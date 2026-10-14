// See include/port/xr_layers.h.

#include "port/xr_layers.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <unordered_map>

#include "Game/Drawable/DrawableObj.h"

extern "C" int PortXrStereoWorld(void);

namespace {

// The field's footprint in world units: the pitch is 41.2 x 24.2 centred on the origin (Field.cpp
// mv3FieldPosition is the half extent), goals on +-X, ground at z = 0. The margin takes in the goals,
// nets and the electric fence around the pitch; anything reaching past it, or taller, is stadium.
const float kFieldHalfX = 27.0f;
const float kFieldHalfY = 17.0f;
// Goals reach 2.7 and the benches 2.3; the signs and rigs at the field's edge (4-6.5) are stadium.
const float kFieldTop = 3.5f;
// Flat stand-ins far under the ground (z = -30 on The Palace) are not part of the field.
const float kFieldBottom = -2.0f;
// Objects centred past this on the camera side (the fence is at about -13.6) are stadium.
const float kCameraSideCutY = -14.5f;

struct Key
{
    const DrawableObject* object;
    unsigned long hash;
    bool operator==(const Key& o) const { return object == o.object && hash == o.hash; }
};
struct KeyHash
{
    size_t operator()(const Key& k) const { return (size_t)k.object ^ ((size_t)k.hash << 1); }
};

// Per object, decided once: stadium or field, and its world bounds at that time with the
// translation they went with (a moved object's bounds are shifted by how far it moved).
struct Info
{
    bool stadium;
    bool crowd;
    bool tallField; // a field object standing up (goals, benches, fence), not the flat pitch
    float mn[3];
    float mx[3];
    float t0[3];
};
std::unordered_map<Key, Info, KeyHash> s_info;

// This frame's stadium matrix handles and their bounds (unbounded: never culled).
struct Mark
{
    float mn[3];
    float mx[3];
    bool bounded;
    bool crowd;
    unsigned cullSerial; // the cull camera the cached answer belongs to
    bool culled;
};
std::unordered_map<uintptr_t, Mark> s_stadium;
// This frame's field (non-stadium) handles, for the self-test switch that drops them from the
// stadium layer.
std::unordered_map<uintptr_t, bool> s_field;
// Camera-side objects and whether the game hides them from its own camera (DoTranslucency), by
// object (kept across frames: the game may draw an object before deciding its translucency for the
// frame, so the latest decision is used, one frame late at worst), and this frame's matrix handles
// of the hidden ones: drawn for the life-size stadium, left out of the normal send and the table.
std::unordered_map<const DrawableObject*, bool> s_cameraSideHidden;
std::unordered_map<uintptr_t, bool> s_normalSkip;
bool s_tableLayer = false;
bool s_eyeSend = false;
// Self-test switches (xr_tune): leave a category out of the eye sends to measure what it costs.
// Self-test probe (xr_tune "probe"): logs where each field object lands in the next table-layer
// view, to name things seen in a snapshot.
struct ProbeInfo
{
    unsigned long hash;
    unsigned long creation;
    unsigned long objectFlags;
    float c[3];
    float ext[3];
};
std::unordered_map<uintptr_t, ProbeInfo> s_probeField;
int s_probeState = 0; // 0 off, 1 armed (collect this frame), 2 log at the table send
bool s_tuneSkipCrowd = false;
bool s_tuneSkipStands = false;
bool s_tuneSkipStadiumField = false;
// Stadium layer economies (PortXrSetStadiumEconomy): stadium objects smaller than this many
// pixels across in an eye are left out (0 = off), and the life-size goals/benches/fence too.
float s_lodMinPx = 0.0f;
float s_eyeHalfWidthPx = 600.0f;
bool s_skipTallField = false;
bool s_bothLayers = false;  // this frame sends the table layer as well (Tabletop · Stadium)
int s_logged = 0;

// The camera of the view being sent, for culling stadium objects it cannot see. Row-vector view
// (v' = v * V, the game's nlMatrix4) and a GX projection (clip = P * v, rows as stored).
bool s_cullOn = false;
float s_V[4][4];
float s_P[4][4];
unsigned s_cullSerial = 1;
unsigned long s_culledThisFrame = 0;
unsigned long s_culledTotal = 0;

bool cull_enabled()
{
    static int s_enabled = -1;
    if (s_enabled < 0)
    {
        const char* e = getenv("STRIKERS_XR_CULL");
        s_enabled = (e != NULL && e[0] == '0') ? 0 : 1;
    }
    return s_enabled != 0;
}

void world_bounds(const DrawableObject* object, float mn[3], float mx[3])
{
    AABBDimensions dims;
    object->GetAABBDimensions(dims, false);
    const nlMatrix4& m = object->GetWorldMatrix();
    for (int k = 0; k < 3; k++)
    {
        mn[k] = 1e30f;
        mx[k] = -1e30f;
    }
    for (int c = 0; c < 8; c++)
    {
        const float x = (c & 1) ? dims.mMax.x : dims.mMin.x;
        const float y = (c & 2) ? dims.mMax.y : dims.mMin.y;
        const float z = (c & 4) ? dims.mMax.z : dims.mMin.z;
        // Row vector times the game's nlMatrix4: translation in the fourth row.
        for (int k = 0; k < 3; k++)
        {
            const float w = x * m.e2[0][k] + y * m.e2[1][k] + z * m.e2[2][k] + m.e2[3][k];
            if (w < mn[k]) mn[k] = w;
            if (w > mx[k]) mx[k] = w;
        }
    }
}

Info classify(const DrawableObject* object)
{
    Info info;
    world_bounds(object, info.mn, info.mx);
    const nlMatrix4& m = object->GetWorldMatrix();
    for (int k = 0; k < 3; k++)
        info.t0[k] = m.e2[3][k];

    const unsigned long creation = object->m_uObjectCreationFlags;
    info.crowd = (creation & 0x20000) != 0;
    // Crowd, skybox and jumbotron are stadium whatever their bounds say.
    if (creation & (0x20000 | 0x100 | 0x10000))
    {
        info.stadium = true;
        return info;
    }
    // Behind the camera-side touchline (game y < 0 is the TV camera's side) is the camera-side bench
    // area the game hides from its own camera (bench 0x2000); the pieces left there (hash 1e1d7fae/f,
    // x +-10, y -15.8, found with xr_tune "probe" in r20) came out as black sawtooth silhouettes
    // beside the Tabletop field in the room. They belong to the stadium, life-size.
    const bool behindCameraTouchline = 0.5f * (info.mn[1] + info.mx[1]) < kCameraSideCutY;
    const bool field = !behindCameraTouchline && info.mn[0] >= -kFieldHalfX && info.mx[0] <= kFieldHalfX &&
                       info.mn[1] >= -kFieldHalfY && info.mx[1] <= kFieldHalfY && info.mx[2] <= kFieldTop &&
                       info.mn[2] >= kFieldBottom;
    if (s_logged < 400)
    {
        s_logged++;
        fprintf(stderr,
                "[xr-layer] %s hash=%08lx flags=%08lx aabb x[%.1f,%.1f] y[%.1f,%.1f] z[%.1f,%.1f]\n",
                field ? "FIELD  " : "STADIUM", (unsigned long)object->m_uHashID, creation, info.mn[0], info.mx[0],
                info.mn[1], info.mx[1], info.mn[2], info.mx[2]);
    }
    info.stadium = !field;
    info.tallField = field && (info.mx[2] - info.mn[2]) > 0.4f;
    return info;
}

// True when the box is wholly outside one side of the current view's frustum. Homogeneous
// half-space tests, so corners behind the camera are handled.
bool outside_frustum(const float mn[3], const float mx[3])
{
    // A little margin: an object grazing the edge is drawn.
    const float pad[3] = { (mx[0] - mn[0]) * 0.05f + 0.5f, (mx[1] - mn[1]) * 0.05f + 0.5f,
                           (mx[2] - mn[2]) * 0.05f + 0.5f };
    int outLeft = 0, outRight = 0, outBottom = 0, outTop = 0, outBehind = 0;
    for (int c = 0; c < 8; c++)
    {
        const float x = (c & 1) ? mx[0] + pad[0] : mn[0] - pad[0];
        const float y = (c & 2) ? mx[1] + pad[1] : mn[1] - pad[1];
        const float z = (c & 4) ? mx[2] + pad[2] : mn[2] - pad[2];
        float e[4];
        for (int k = 0; k < 4; k++)
            e[k] = x * s_V[0][k] + y * s_V[1][k] + z * s_V[2][k] + s_V[3][k];
        const float cx = s_P[0][0] * e[0] + s_P[0][1] * e[1] + s_P[0][2] * e[2] + s_P[0][3] * e[3];
        const float cy = s_P[1][0] * e[0] + s_P[1][1] * e[1] + s_P[1][2] * e[2] + s_P[1][3] * e[3];
        const float cw = s_P[3][0] * e[0] + s_P[3][1] * e[1] + s_P[3][2] * e[2] + s_P[3][3] * e[3];
        outLeft += (cx + cw < 0.0f);
        outRight += (cw - cx < 0.0f);
        outBottom += (cy + cw < 0.0f);
        outTop += (cw - cy < 0.0f);
        outBehind += (cw <= 0.0f);
    }
    return outLeft == 8 || outRight == 8 || outBottom == 8 || outTop == 8 || outBehind == 8;
}

// True when the box would be smaller than s_lodMinPx across in the current eye: far-off details.
bool too_small(const float mn[3], const float mx[3])
{
    const float c[3] = { 0.5f * (mn[0] + mx[0]), 0.5f * (mn[1] + mx[1]), 0.5f * (mn[2] + mx[2]) };
    const float dx = mx[0] - mn[0], dy = mx[1] - mn[1], dz = mx[2] - mn[2];
    const float radius = 0.5f * sqrtf(dx * dx + dy * dy + dz * dz);
    float e[3];
    for (int k = 0; k < 3; k++)
        e[k] = c[0] * s_V[0][k] + c[1] * s_V[1][k] + c[2] * s_V[2][k] + s_V[3][k];
    const float depth = -e[2] - radius; // nearest point, along the view direction
    if (depth <= 0.5f)
        return false;
    const float px = 2.0f * radius / depth * s_P[0][0] * s_eyeHalfWidthPx;
    return px < s_lodMinPx;
}

} // namespace

void PortXrSetStadiumEconomy(float minPx, float eyeHalfWidthPx, int skipTallField, int bothLayers)
{
    s_lodMinPx = minPx;
    s_eyeHalfWidthPx = eyeHalfWidthPx;
    s_skipTallField = skipTallField != 0;
    s_bothLayers = bothLayers != 0;
}

void PortXrClassifyAndMark(const DrawableObject* object, uintptr_t matrixHandle)
{
    if (object == NULL || !PortXrStereoWorld())
        return;
    if (!s_cameraSideHidden.empty())
    {
        std::unordered_map<const DrawableObject*, bool>::const_iterator hidden = s_cameraSideHidden.find(object);
        if (hidden != s_cameraSideHidden.end() && hidden->second)
            s_normalSkip[matrixHandle] = true;
    }
    const Key key = { object, object->m_uHashID };
    std::unordered_map<Key, Info, KeyHash>::iterator it = s_info.find(key);
    if (it == s_info.end())
        it = s_info.insert(std::make_pair(key, classify(object))).first;
    const Info& info = it->second;
    if (!info.stadium)
    {
        if (s_tuneSkipStadiumField || (s_skipTallField && info.tallField))
            s_field[matrixHandle] = true;
        if (s_probeState != 0)
        {
            ProbeInfo p;
            p.hash = object->m_uHashID;
            p.creation = object->m_uObjectCreationFlags;
            p.objectFlags = object->m_uObjectFlags;
            for (int k = 0; k < 3; k++)
            {
                p.c[k] = 0.5f * (info.mn[k] + info.mx[k]);
                p.ext[k] = info.mx[k] - info.mn[k];
            }
            s_probeField[matrixHandle] = p;
        }
        return;
    }
    const nlMatrix4& m = object->GetWorldMatrix();
    Mark mark;
    mark.crowd = info.crowd;
    for (int k = 0; k < 3; k++)
    {
        const float d = m.e2[3][k] - info.t0[k];
        mark.mn[k] = info.mn[k] + d;
        mark.mx[k] = info.mx[k] + d;
    }
    mark.bounded = true;
    mark.cullSerial = 0;
    mark.culled = false;
    s_stadium[matrixHandle] = mark;
}

bool PortXrLayersActive() { return PortXrStereoWorld() != 0; }

void PortXrMarkStadiumHandle(uintptr_t matrixHandle)
{
    if (!PortXrStereoWorld())
        return;
    Mark mark;
    memset(&mark, 0, sizeof(mark));
    mark.bounded = false;
    s_stadium[matrixHandle] = mark;
}

void PortXrSetEyeSend(bool eyeSend) { s_eyeSend = eyeSend; }

void PortXrTuneSkip(int crowd, int stands, int stadiumField)
{
    s_tuneSkipCrowd = crowd != 0;
    s_tuneSkipStands = stands != 0;
    s_tuneSkipStadiumField = stadiumField != 0;
}

void PortXrSetCullCamera(const float* view16, const float* proj16)
{
    if (view16 == NULL || proj16 == NULL || !cull_enabled())
    {
        s_cullOn = false;
        return;
    }
    memcpy(s_V, view16, sizeof(s_V));
    memcpy(s_P, proj16, sizeof(s_P));
    s_cullOn = true;
    ++s_cullSerial;
}

void PortXrProbe() { s_probeState = 1; }

// At a table-layer draw with the probe armed: where this field object's centre lands (NDC).
static void probe_field(uintptr_t matrixHandle)
{
    std::unordered_map<uintptr_t, ProbeInfo>::iterator it = s_probeField.find(matrixHandle);
    if (it == s_probeField.end())
        return;
    const ProbeInfo& p = it->second;
    float e[4];
    for (int k = 0; k < 4; k++)
        e[k] = p.c[0] * s_V[0][k] + p.c[1] * s_V[1][k] + p.c[2] * s_V[2][k] + s_V[3][k];
    const float cx = s_P[0][0] * e[0] + s_P[0][1] * e[1] + s_P[0][2] * e[2] + s_P[0][3] * e[3];
    const float cy = s_P[1][0] * e[0] + s_P[1][1] * e[1] + s_P[1][2] * e[2] + s_P[1][3] * e[3];
    const float cw = s_P[3][0] * e[0] + s_P[3][1] * e[1] + s_P[3][2] * e[2] + s_P[3][3] * e[3];
    fprintf(stderr,
            "[xr-probe] hash=%08lx flags=%08lx objflags=%08lx centre (%.1f %.1f %.1f) size (%.1f %.1f %.1f) ndc "
            "(%.2f %.2f)\n",
            p.hash, p.creation, p.objectFlags, p.c[0], p.c[1], p.c[2], p.ext[0], p.ext[1], p.ext[2],
            cw != 0.0f ? cx / cw : 0.0f, cw != 0.0f ? cy / cw : 0.0f);
    s_probeField.erase(it); // once per object
}

void PortXrNoteCameraSide(const DrawableObject* object, bool hiddenForCamera)
{
    s_cameraSideHidden[object] = hiddenForCamera;
}

bool PortXrSkipPacketDraw(uintptr_t matrixHandle)
{
    if (s_probeState == 2 && s_eyeSend && s_tableLayer && s_cullOn)
        probe_field(matrixHandle);
    // Camera-side objects the game hides from its camera: kept for the life-size stadium only, left
    // out of the broadcast view and of the Tabletop field (r21 had the hidden centre-line sign back
    // on the table in Tabletop · Stadium).
    if ((!s_eyeSend || s_tableLayer) && !s_normalSkip.empty() && s_normalSkip.count(matrixHandle) != 0)
        return true;
    // The stadium layer's life-size field: all of it (self-test), or the standing parts when the
    // table carries the game anyway.
    if (s_eyeSend && !s_tableLayer && !s_field.empty() && (s_tuneSkipStadiumField || s_bothLayers) &&
        s_field.count(matrixHandle) != 0)
        return true;
    if (s_stadium.empty())
        return false;
    std::unordered_map<uintptr_t, Mark>::iterator it = s_stadium.find(matrixHandle);
    if (it == s_stadium.end())
        return false;
    if (s_tableLayer)
        return true;
    Mark& mark = it->second;
    if (s_eyeSend && ((mark.crowd && s_tuneSkipCrowd) || (!mark.crowd && s_tuneSkipStands)))
        return true;
    if (!s_cullOn || !mark.bounded)
        return false;
    if (mark.cullSerial != s_cullSerial)
    {
        mark.cullSerial = s_cullSerial;
        mark.culled = outside_frustum(mark.mn, mark.mx) ||
                      (s_eyeSend && !s_tableLayer && s_lodMinPx > 0.0f && !mark.crowd && too_small(mark.mn, mark.mx));
        if (mark.culled)
            ++s_culledThisFrame;
    }
    return mark.culled;
}

void PortXrSetTableLayer(bool table) { s_tableLayer = table; }

unsigned long PortXrLayersTakeCulled()
{
    const unsigned long n = s_culledTotal;
    s_culledTotal = 0;
    return n;
}

void PortXrLayersEndFrame()
{
    // The probe collects one frame's field objects, logs them at that frame's table send.
    if (s_probeState == 2)
    {
        s_probeState = 0;
        s_probeField.clear();
    }
    else if (s_probeState == 1)
    {
        s_probeState = 2;
    }
    s_stadium.clear();
    s_field.clear();
    s_normalSkip.clear();
    if (!PortXrStereoWorld())
        s_cameraSideHidden.clear(); // objects come and go with matches; start fresh outside 3D
    s_eyeSend = false;
    s_tableLayer = false;
    s_cullOn = false;
    s_culledTotal += s_culledThisFrame;
    s_culledThisFrame = 0;
}
