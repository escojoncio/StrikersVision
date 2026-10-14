package io.github.newcoke.strikers;

import android.app.Activity;
import android.app.PendingIntent;
import android.content.Intent;
import android.content.pm.PackageInstaller;

import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.Locale;
import java.util.function.IntConsumer;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

/**
 * strikers-quest r41: a newer release on the project's GitHub page, and installing it from the
 * launcher panel (LobbyActivity). The panel asks GitHub for the latest release when it opens; a
 * newer one shows "Update now", which downloads its .apk (checked against the size and SHA-256
 * GitHub lists for it) and hands it to the system installer. Android itself only installs it over
 * this app when it is signed with the same key and has a higher versionCode.
 */
final class Updater {
    static final String REPO = "LanceSweatpants/Strikers-Quest-MR";
    private static final String LATEST = "https://api.github.com/repos/" + REPO + "/releases/latest";
    // Only this repository's own release files are downloaded.
    private static final String DOWNLOAD_PREFIX = "https://github.com/" + REPO + "/releases/download/";
    static final String ACTION_INSTALL_STATUS = "io.github.newcoke.strikers.INSTALL_STATUS";
    private static final Pattern VERSION = Pattern.compile("(\\d+)\\.(\\d+)\\.(\\d+)");

    private Updater() {}

    static final class Release {
        final String tag;
        final String name;
        final String apkUrl;
        final long size;
        final String sha256;  // lower-case hex, or "" when GitHub lists none

        Release(String tag, String name, String apkUrl, long size, String sha256) {
            this.tag = tag;
            this.name = name;
            this.apkUrl = apkUrl;
            this.size = size;
            this.sha256 = sha256;
        }
    }

    /** The latest release when it is newer than `ownVersion` (a versionName), else null. Network. */
    static Release newer(String ownVersion) {
        try {
            HttpURLConnection c = (HttpURLConnection) new URL(LATEST).openConnection();
            c.setConnectTimeout(6000);
            c.setReadTimeout(6000);
            c.setRequestProperty("Accept", "application/vnd.github+json");
            c.setRequestProperty("User-Agent", "QuestStrikers");
            if (c.getResponseCode() != 200) return null;
            JSONObject j;
            try (InputStream in = c.getInputStream()) {
                j = new JSONObject(readAll(in));
            }
            if (j.optBoolean("draft") || j.optBoolean("prerelease")) return null;
            String tag = j.getString("tag_name");
            int[] theirs = numbers(tag), ours = numbers(ownVersion);
            if (theirs == null || ours == null || compare(theirs, ours) <= 0) return null;
            JSONArray assets = j.getJSONArray("assets");
            for (int i = 0; i < assets.length(); i++) {
                JSONObject a = assets.getJSONObject(i);
                String url = a.optString("browser_download_url", "");
                if (!a.optString("name", "").toLowerCase(Locale.ROOT).endsWith(".apk")
                        || !url.startsWith(DOWNLOAD_PREFIX)) {
                    continue;
                }
                String digest = a.optString("digest", "");
                String sha = digest.startsWith("sha256:") ? digest.substring(7).toLowerCase(Locale.ROOT) : "";
                return new Release(tag, j.optString("name", tag), url, a.optLong("size", -1), sha);
            }
        } catch (IOException | JSONException | RuntimeException e) {
            // Offline, GitHub unreachable or rate-limited: no update shown, nothing else changes.
        }
        return null;
    }

    /** The release's .apk into `dir`, with progress 0..100; checked against its size and SHA-256. */
    static File download(Release r, File dir, IntConsumer progress) throws IOException {
        //noinspection ResultOfMethodCallIgnored
        dir.mkdirs();
        File apk = new File(dir, "QuestStrikers-update.apk");
        //noinspection ResultOfMethodCallIgnored
        apk.delete();
        HttpURLConnection c = (HttpURLConnection) new URL(r.apkUrl).openConnection();
        c.setConnectTimeout(10000);
        c.setReadTimeout(20000);
        c.setRequestProperty("User-Agent", "QuestStrikers");
        if (c.getResponseCode() != 200) throw new IOException("GitHub answered " + c.getResponseCode());
        MessageDigest sha;
        try {
            sha = MessageDigest.getInstance("SHA-256");
        } catch (NoSuchAlgorithmException e) {
            throw new IOException(e);
        }
        long total = r.size > 0 ? r.size : c.getContentLengthLong();
        long done = 0;
        int last = -1;
        byte[] buf = new byte[64 * 1024];
        try (InputStream in = c.getInputStream(); OutputStream out = new FileOutputStream(apk)) {
            int n;
            while ((n = in.read(buf)) > 0) {
                out.write(buf, 0, n);
                sha.update(buf, 0, n);
                done += n;
                int pct = total > 0 ? (int) Math.min(100, done * 100 / total) : 0;
                if (pct != last) {
                    last = pct;
                    progress.accept(pct);
                }
            }
        }
        if (r.size > 0 && done != r.size) throw new IOException("the download was cut short");
        if (!r.sha256.isEmpty() && !r.sha256.equals(hex(sha.digest()))) {
            throw new IOException("the download does not match the release");
        }
        return apk;
    }

    /** Hands the APK to the system installer; the result comes back as ACTION_INSTALL_STATUS. */
    static void install(Activity activity, File apk) throws IOException {
        PackageInstaller installer = activity.getPackageManager().getPackageInstaller();
        PackageInstaller.SessionParams params =
                new PackageInstaller.SessionParams(PackageInstaller.SessionParams.MODE_FULL_INSTALL);
        params.setAppPackageName(activity.getPackageName());
        params.setSize(apk.length());
        int id = installer.createSession(params);
        try (PackageInstaller.Session session = installer.openSession(id)) {
            try (InputStream in = new FileInputStream(apk);
                 OutputStream out = session.openWrite("base.apk", 0, apk.length())) {
                byte[] buf = new byte[64 * 1024];
                int n;
                while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
                session.fsync(out);
            }
            Intent status = new Intent(activity, LobbyActivity.class).setAction(ACTION_INSTALL_STATUS);
            PendingIntent pending = PendingIntent.getActivity(activity, 0, status,
                    PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_MUTABLE);
            session.commit(pending.getIntentSender());
        }
    }

    static int[] numbers(String version) {
        Matcher m = VERSION.matcher(version);
        if (!m.find()) return null;
        return new int[] {Integer.parseInt(m.group(1)), Integer.parseInt(m.group(2)), Integer.parseInt(m.group(3))};
    }

    private static int compare(int[] a, int[] b) {
        for (int i = 0; i < 3; i++) if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
        return 0;
    }

    private static String readAll(InputStream in) throws IOException {
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        byte[] buf = new byte[16 * 1024];
        int n;
        while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
        return new String(out.toByteArray(), StandardCharsets.UTF_8);
    }

    private static String hex(byte[] b) {
        StringBuilder sb = new StringBuilder();
        for (byte x : b) sb.append(String.format(Locale.ROOT, "%02x", x & 0xFF));
        return sb.toString();
    }
}
