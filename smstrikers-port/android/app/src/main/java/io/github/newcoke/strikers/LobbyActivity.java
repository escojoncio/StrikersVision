package io.github.newcoke.strikers;

import android.app.Activity;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.pm.PackageInstaller;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.provider.Settings;
import android.text.InputFilter;
import android.text.InputType;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.view.inputmethod.EditorInfo;
import android.widget.Button;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.security.SecureRandom;
import java.util.List;

/**
 * strikers-quest r39: the panel in front of the game. Play offline, host an online match (a
 * four-letter room code to tell your friend), or join one by typing its code. The game itself is
 * {@link StrikersActivity}, immersive; this is a flat Quest panel that starts it with the room
 * code (extra "room", "" for offline), which the game hands to its network layer (STRIKERS_ROOM).
 * In a room, both players pick Grudge Match and start: the match begins once both are in.
 *
 * <p>r39b: this panel runs in the app's main process and the game in a process of its own
 * (":game", see StrikersActivity.onDestroy). The "all files access" switch is asked for from here,
 * before the game is started.
 */
public class LobbyActivity extends Activity {

    static final String EXTRA_ROOM = "room";
    private static final String PREFS = "lobby";
    private static final String LAST_CODE = "lastCode";
    // strikers-quest r39b: the code Host showed last, so it shows the same one after a relaunch.
    private static final String LAST_HOSTED = "lastHosted";
    // No I, O, 0 or 1: a code is read out loud or off a headset.
    private static final String CODE_LETTERS = "ABCDEFGHJKLMNPQRSTUVWXYZ";
    private static final int BG = Color.rgb(16, 20, 28);
    private static final int CARD = Color.rgb(30, 38, 52);
    private static final int ACCENT = Color.rgb(255, 196, 0);
    private static final int WARN = Color.rgb(255, 150, 140);
    private static final int MUTED = Color.rgb(150, 160, 175);

    private LinearLayout page;
    private SharedPreferences prefs;
    private TextView accessNote;  // r39b: under the buttons while the file access switch is off
    private int chooserToken;     // r41: the game file list being filled (a newer one wins)
    private Updater.Release update;  // r41: a newer release on GitHub, null when there is none
    private boolean onHome;          // r41: the home page is showing (the update check redraws it)

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        prefs = getSharedPreferences(PREFS, MODE_PRIVATE);
        page = new LinearLayout(this);
        page.setOrientation(LinearLayout.VERTICAL);
        page.setGravity(Gravity.CENTER_HORIZONTAL);
        page.setBackgroundColor(BG);
        int pad = dp(28);
        page.setPadding(pad, pad, pad, pad);
        // r41: scrolls, for the game file list and a last error on a short panel.
        ScrollView scroll = new ScrollView(this);
        scroll.setBackgroundColor(BG);
        scroll.setFillViewport(true);
        scroll.addView(page, new ScrollView.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT));
        setContentView(scroll);
        showHome();
        checkForUpdate();
    }

    private void showHome() {
        page.removeAllViews();
        onHome = true;
        title("Quest 3 Strikers", 30);
        if (update != null) {
            note(shortName(update) + " is out.").setTextColor(ACCENT);
            page.addView(button("Update now", true, v -> startUpdate()));
        }
        showLastError();
        note("Quest alpha. Online: both players pick Grudge Match in the same room.");
        page.addView(button("Play offline", true, v -> start("")));
        page.addView(button("Host an online match", false, v -> showHost(hostedCode())));
        page.addView(button("Join with a code", false, v -> showJoin()));
        page.addView(button("Choose game file", false, v -> showChooser(null, null)));
        TextView game = note(gameStatus());
        game.setTextSize(14);
        TextView version = note(versionText());
        version.setTextSize(12);
        version.setTextColor(MUTED);
    }

    /** strikers-quest r41: GitHub's latest release, once per panel start, off the UI thread. */
    private void checkForUpdate() {
        final String own = versionName();
        new Thread(() -> {
            final Updater.Release r = Updater.newer(own);
            if (r == null) return;
            runOnUiThread(() -> {
                update = r;
                if (onHome) showHome();
            });
        }, "strikers-update-check").start();
    }

    /** strikers-quest r41: download the newer release and hand it to the system installer. */
    private void startUpdate() {
        final Updater.Release r = update;
        if (r == null) return;
        // The system's "install unknown apps" switch for this app, the first time. Where the
        // headset has no such screen, the installer itself asks.
        if (!getPackageManager().canRequestPackageInstalls()) {
            try {
                startActivity(new Intent(Settings.ACTION_MANAGE_UNKNOWN_APP_SOURCES,
                        Uri.parse("package:" + getPackageName())));
                showHome();
                note("Allow this app to install its update, then come back here and press Update now"
                        + " again.").setTextColor(WARN);
                return;
            } catch (Exception e) {
                // fall through to the installer
            }
        }
        onHome = false;
        page.removeAllViews();
        title("Updating to " + shortName(r), 24);
        final TextView status = note("Downloading... 0%");
        page.addView(button("Back", false, v -> showHome()));
        new Thread(() -> {
            try {
                File apk = Updater.download(r, new File(getCacheDir(), "update"),
                        pct -> runOnUiThread(() -> status.setText("Downloading... " + pct + "%")));
                runOnUiThread(() -> status.setText("Installing. Confirm on the next screen: the app closes,"
                        + " updates, and you open it again."));
                Updater.install(this, apk);
            } catch (IOException | RuntimeException e) {
                runOnUiThread(() -> status.setText("The update didn't work (" + e.getMessage() + ").\nCheck"
                        + " the Quest's internet connection and try again, or get it from the GitHub page."));
            }
        }, "strikers-update").start();
    }

    /** strikers-quest r41: the system installer's answer (Updater.install). */
    @Override
    protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        if (intent == null || !Updater.ACTION_INSTALL_STATUS.equals(intent.getAction())) return;
        int status = intent.getIntExtra(PackageInstaller.EXTRA_STATUS, PackageInstaller.STATUS_FAILURE);
        if (status == PackageInstaller.STATUS_PENDING_USER_ACTION) {
            Intent confirm = intent.getParcelableExtra(Intent.EXTRA_INTENT);  // the "Install?" screen
            if (confirm != null) startActivity(confirm);
        } else if (status != PackageInstaller.STATUS_SUCCESS) {
            String msg = intent.getStringExtra(PackageInstaller.EXTRA_STATUS_MESSAGE);
            showHome();
            note("The update wasn't installed" + (msg != null ? ": " + msg : ".")).setTextColor(WARN);
        }
    }

    /** "alpha 1.2" out of "Quest 3 Strikers - alpha 1.2". */
    private static String shortName(Updater.Release r) {
        String name = r.name.replaceFirst("(?i)^quest 3 strikers\\s*[-–—:]\\s*", "");
        return name.isEmpty() ? r.tag : name.substring(0, 1).toUpperCase(java.util.Locale.ROOT) + name.substring(1);
    }

    /**
     * strikers-quest r41: the message the game closed with last time (fatal.c writes it; under the
     * immersive view its own message box is never seen), with an OK that clears it.
     */
    private void showLastError() {
        final File f = new File(StrikersHome.homeDir(this), "last-error.txt");
        if (!f.isFile()) return;
        String text = readText(f, 1500);
        String[] lines = text.split("\n");
        StringBuilder shown = new StringBuilder();
        for (int i = 0; i < lines.length && i < 12; i++) shown.append(i > 0 ? "\n" : "").append(lines[i]);
        TextView t = note("The game closed last time with this message:\n\n" + shown);
        t.setTextColor(WARN);
        t.setTextSize(14);
        page.addView(button("OK", false, v -> {
            //noinspection ResultOfMethodCallIgnored
            f.delete();
            showHome();
        }));
    }

    /** strikers-quest r41: which game file a match would start with, for the home page. */
    private String gameStatus() {
        if (!StrikersHome.hasAllFilesAccess()) {
            return "Game file: the app needs file access to look for it.";
        }
        String path = StrikersHome.describeGameData(this);
        if (path == null) return "No game file found yet. Press \"Choose game file\".";
        File f = new File(path);
        if (f.isDirectory()) return "Game: the extracted disc in " + GameFile.shortPath(f);
        GameFile.Found found = GameFile.check(f);
        return (found.usable ? "Game: " + found.label : "Game file problem: " + found.problem)
                + "\n" + GameFile.shortPath(f);
    }

    /** Null when the game has a file it can read; otherwise what is wrong, in one sentence. */
    private String gameProblem() {
        String path = StrikersHome.describeGameData(this);
        if (path == null) return "Your game file wasn't found on this Quest.";
        File f = new File(path);
        if (f.isDirectory()) return null;
        GameFile.Found found = GameFile.check(f);
        return found.usable ? null : GameFile.shortPath(f) + ": " + found.problem;
    }

    /**
     * strikers-quest r41: every .iso, .rvz and .wia on the headset, checked, to tap the one to play.
     * code != null: the match the player asked for, started once a file is picked.
     */
    private void showChooser(String code, String reason) {
        if (!StrikersHome.hasAllFilesAccess() && askForFileAccess(this)) {
            showHome();
            accessNote = note("Turn on \"Allow access to manage all files\" so the game can find your"
                    + " game file, then come back here and press the same button again.");
            return;
        }
        page.removeAllViews();
        onHome = false;
        title("Choose your game file", 24);
        if (reason != null) note(reason).setTextColor(WARN);
        final TextView status = note("Looking for game files on this Quest...");
        final LinearLayout list = new LinearLayout(this);
        list.setOrientation(LinearLayout.VERTICAL);
        list.setGravity(Gravity.CENTER_HORIZONTAL);
        page.addView(list, wrap());
        page.addView(button("Look again", false, v -> showChooser(code, null)));
        page.addView(button("Back", false, v -> showHome()));
        final int token = ++chooserToken;
        new Thread(() -> {
            final List<GameFile.Found> found = GameFile.scan();
            runOnUiThread(() -> {
                if (token != chooserToken || list.getParent() != page) return;  // left this page
                if (found.isEmpty()) {
                    status.setText("No .iso, .rvz or .wia file found on this Quest.\n\nOn your PC, open"
                            + " SideQuest, click the folder icon, open Download, and drag your game file"
                            + " in. Then press Look again.");
                    return;
                }
                status.setText("Tap your game file:");
                String chosen = GameFile.chosen(this);
                for (GameFile.Found f : found) {
                    String where = GameFile.shortPath(f.file) + " · " + GameFile.size(f.file.length());
                    boolean isChosen = f.file.getAbsolutePath().equals(chosen);
                    String text = f.file.getName() + (isChosen ? "   (chosen)" : "") + "\n"
                            + (f.usable ? f.label : f.label + ": " + f.problem) + "\n" + where;
                    list.addView(row(text, f.usable, f.usable ? v -> {
                        GameFile.choose(this, f.file.getAbsolutePath());
                        if (code != null) start(code); else showHome();
                    } : null));
                }
            });
        }, "strikers-scan").start();
    }

    /** strikers-quest r39b: the code Host showed last, or a fresh one the first time. */
    private String hostedCode() {
        String code = clean(prefs.getString(LAST_HOSTED, ""));
        return code.length() >= 3 ? code : newCode();
    }

    private void showHost(String code) {
        // strikers-quest r39b: remembered as soon as it is shown, since that is when it gets read
        // out to the friend. Host shows it again until "New code".
        prefs.edit().putString(LAST_HOSTED, code).apply();
        page.removeAllViews();
        onHome = false;
        title("Your room code", 24);
        TextView big = new TextView(this);
        big.setText(spaced(code));
        big.setTextColor(ACCENT);
        big.setTypeface(Typeface.MONOSPACE, Typeface.BOLD);
        big.setTextSize(64);
        big.setGravity(Gravity.CENTER);
        big.setPadding(0, dp(8), 0, dp(8));
        page.addView(big, wrap());
        note("Tell your friend this code. Both of you: Grudge Match, pick your team, start.");
        page.addView(button("Start", true, v -> start(code)));
        page.addView(button("New code", false, v -> showHost(newCode())));
        page.addView(button("Back", false, v -> showHome()));
    }

    private void showJoin() {
        page.removeAllViews();
        onHome = false;
        title("Join a room", 24);
        note("Type the code your friend sees.");
        EditText field = new EditText(this);
        field.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_CAP_CHARACTERS
                | InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS);
        field.setFilters(new InputFilter[] {new InputFilter.AllCaps(), new InputFilter.LengthFilter(8)});
        field.setTextSize(40);
        field.setTypeface(Typeface.MONOSPACE, Typeface.BOLD);
        field.setTextColor(Color.WHITE);
        field.setHintTextColor(Color.GRAY);
        field.setHint("CODE");
        field.setGravity(Gravity.CENTER);
        field.setImeOptions(EditorInfo.IME_ACTION_GO);
        field.setSingleLine(true);
        field.setText(prefs.getString(LAST_CODE, ""));
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(dp(360), ViewGroup.LayoutParams.WRAP_CONTENT);
        lp.topMargin = dp(8);
        page.addView(field, lp);
        TextView problem = note("");
        Runnable join = () -> {
            String code = clean(field.getText().toString());
            if (code.length() < 3) {
                problem.setText("A code is at least 3 letters or numbers.");
                return;
            }
            start(code);
        };
        field.setOnEditorActionListener((v, actionId, event) -> {
            join.run();
            return true;
        });
        page.addView(button("Join", true, v -> join.run()));
        page.addView(button("Back", false, v -> showHome()));
        field.requestFocus();
    }

    private void start(String code) {
        if (!code.isEmpty()) prefs.edit().putString(LAST_CODE, code).apply();
        // strikers-quest r39b: the disc usually sits in Download, which takes "all files access". A
        // friend who only installed the APK has not given it yet: open that switch from here and
        // stay on this page, rather than start the game only for it to open the switch and close.
        // Not when the disc is in the app's own folder, which needs no permission.
        // r41: asked whenever the disc is not in the app's own folder (a picked file is elsewhere).
        if (!StrikersHome.hasAllFilesAccess()
                && !StrikersHome.hasGameDataInHome(StrikersHome.homeDir(this))
                && askForFileAccess(this)) {
            if (accessNote == null || accessNote.getParent() != page) accessNote = note("");
            accessNote.setText("Turn on \"Allow access to manage all files\" so the game can read your"
                    + " disc file, then come back here and press the same button again.");
            return;
        }
        // r41: no game file, or one the game cannot read: say so and show the list, rather than
        // start a game that closes at once (alpha 1: "it just closes").
        String problem = gameProblem();
        if (problem != null) {
            showChooser(code, problem);
            return;
        }
        Intent game = new Intent(this, StrikersActivity.class);
        game.putExtra(EXTRA_ROOM, code);
        startActivity(game);
        finish();
    }

    /**
     * strikers-quest: the "all files access" switch for this app; false when the system has no such
     * screen. r39b: moved here from StrikersActivity, which still uses it when started directly.
     */
    static boolean askForFileAccess(Activity activity) {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.R) return false;
        try {
            activity.startActivity(new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                    Uri.parse("package:" + activity.getPackageName())));
            return true;
        } catch (Exception e) {
            try {
                activity.startActivity(new Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION));
                return true;
            } catch (Exception e2) {
                return false;
            }
        }
    }

    private static String newCode() {
        SecureRandom r = new SecureRandom();
        StringBuilder sb = new StringBuilder();
        for (int i = 0; i < 4; i++) sb.append(CODE_LETTERS.charAt(r.nextInt(CODE_LETTERS.length())));
        return sb.toString();
    }

    static String clean(String s) {
        StringBuilder sb = new StringBuilder();
        for (char c : s.toUpperCase().toCharArray()) {
            if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) sb.append(c);
            if (sb.length() == 8) break;
        }
        return sb.toString();
    }

    private static String spaced(String code) {
        StringBuilder sb = new StringBuilder();
        for (int i = 0; i < code.length(); i++) {
            if (i > 0) sb.append(' ');
            sb.append(code.charAt(i));
        }
        return sb.toString();
    }

    private void title(String text, int sp) {
        TextView t = new TextView(this);
        t.setText(text);
        t.setTextColor(Color.WHITE);
        t.setTextSize(sp);
        t.setTypeface(Typeface.DEFAULT_BOLD);
        t.setGravity(Gravity.CENTER);
        page.addView(t, wrap());
    }

    private TextView note(String text) {
        TextView t = new TextView(this);
        t.setText(text);
        t.setTextColor(Color.rgb(190, 200, 215));
        t.setTextSize(16);
        t.setGravity(Gravity.CENTER);
        t.setPadding(0, dp(6), 0, dp(14));
        page.addView(t, wrap());
        return t;
    }

    private Button button(String text, boolean primary, View.OnClickListener onClick) {
        Button b = new Button(this);
        b.setText(text);
        b.setAllCaps(false);
        b.setTextSize(20);
        b.setTextColor(primary ? Color.BLACK : Color.WHITE);
        GradientDrawable bg = new GradientDrawable();
        bg.setCornerRadius(dp(14));
        bg.setColor(primary ? ACCENT : CARD);
        b.setBackground(bg);
        b.setOnClickListener(onClick);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(dp(360), dp(64));
        lp.topMargin = dp(10);
        b.setLayoutParams(lp);
        return b;
    }

    /** r41: one file in the game file list; onClick null = shown but not pickable. */
    private Button row(String text, boolean usable, View.OnClickListener onClick) {
        Button b = new Button(this);
        b.setText(text);
        b.setAllCaps(false);
        b.setTextSize(15);
        b.setGravity(Gravity.START | Gravity.CENTER_VERTICAL);
        b.setTextColor(usable ? Color.WHITE : MUTED);
        GradientDrawable bg = new GradientDrawable();
        bg.setCornerRadius(dp(12));
        bg.setColor(CARD);
        if (usable) bg.setStroke(dp(2), ACCENT);
        b.setBackground(bg);
        int p = dp(14);
        b.setPadding(p, p, p, p);
        b.setEnabled(onClick != null);
        b.setOnClickListener(onClick);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(dp(620), ViewGroup.LayoutParams.WRAP_CONTENT);
        lp.topMargin = dp(8);
        b.setLayoutParams(lp);
        return b;
    }

    private String versionName() {
        try {
            return getPackageManager().getPackageInfo(getPackageName(), 0).versionName;
        } catch (Exception e) {
            return "";
        }
    }

    private String versionText() {
        String v = versionName();
        return v.isEmpty() ? "" : "Version " + v;
    }

    private static String readText(File f, int max) {
        try (InputStream in = new FileInputStream(f)) {
            byte[] b = new byte[max];
            int n = in.read(b);
            return n > 0 ? new String(b, 0, n, StandardCharsets.UTF_8).trim() : "";
        } catch (IOException e) {
            return "";
        }
    }

    private LinearLayout.LayoutParams wrap() {
        return new LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT);
    }

    private int dp(int v) {
        return Math.round(v * getResources().getDisplayMetrics().density);
    }
}
