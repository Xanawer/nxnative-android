// The app's activity (dm/android/app, docs/android/PLAN.md 1.6): NativeActivity, which runs libdmhost.so (dm/android/front/front_android.c), plus the software keyboard's input box.
// front_text_input (front.h) calls showTextInput on a host-owned thread; the box is an AlertDialog with an EditText, so the system keyboard types into it as into any text field (its committed
// text, composition and suggestions included), and its answer goes back through nativeTextDone, which front_android.c registers in onCreate. The box keeps OK disabled until the text reaches
// the game's minimum length, as the Switch applet does (front_mac.m brings its box back instead). Built by build_app.sh (javac, d8) into the APK's classes.dex
package org.nxnative.host;

import android.app.AlertDialog;
import android.app.NativeActivity;
import android.content.DialogInterface;
import android.text.Editable;
import android.text.InputFilter;
import android.text.InputType;
import android.text.TextWatcher;
import android.view.KeyEvent;
import android.view.WindowManager;
import android.view.inputmethod.EditorInfo;
import android.widget.Button;
import android.widget.EditText;
import android.widget.FrameLayout;
import android.widget.TextView;
import java.nio.charset.StandardCharsets;

public class HostActivity extends NativeActivity {
    // The box's answer, once per box: ok false for a cancel. The text goes as UTF-8, and the strings come in as UTF-8, because JNI's own strings are modified UTF-8
    private static native void nativeTextDone(long token, boolean ok, byte[] utf8);

    private static String str(byte[] b) { return b == null ? "" : new String(b, StandardCharsets.UTF_8); }

    // Any thread (front_android.c calls it from a host-owned one); the box is made on the UI thread. maxChars and minChars count UTF-16 units, 0 for none
    public void showTextInput(final long token, byte[] title, byte[] message, byte[] hint, byte[] initial, final int maxChars, final int minChars) {
        final String t = str(title), m = str(message), h = str(hint), i = str(initial);
        runOnUiThread(new Runnable() { public void run() { show(token, t, m, h, i, maxChars, minChars); } });
    }

    private void show(final long token, String title, String message, String hint, String initial, int maxChars, final int minChars) {
        // The window going away first (finishing, or destroyed before the runnable ran): no box, a cancel (d.show would throw on the UI thread, which nothing here catches)
        if (isFinishing() || isDestroyed()) { nativeTextDone(token, false, null); return; }
        final boolean[] done = { false };
        final EditText f = new EditText(this);
        f.setSingleLine(true);
        f.setInputType(InputType.TYPE_CLASS_TEXT);
        f.setImeOptions(EditorInfo.IME_ACTION_DONE | EditorInfo.IME_FLAG_NO_EXTRACT_UI);   // the box stays in view in landscape, rather than a full-screen editor
        if (maxChars > 0) f.setFilters(new InputFilter[] { new InputFilter.LengthFilter(maxChars) });
        f.setText(initial);
        f.setHint(hint);
        f.selectAll();
        FrameLayout pad = new FrameLayout(this);
        int px = (int) (20 * getResources().getDisplayMetrics().density);
        pad.setPadding(px, px / 2, px, 0);
        pad.addView(f);
        AlertDialog.Builder b = new AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert)
            .setTitle(title.isEmpty() ? "Input" : title)
            .setView(pad)
            .setPositiveButton(android.R.string.ok, null)
            .setNegativeButton(android.R.string.cancel, null);
        if (!message.isEmpty()) b.setMessage(message);
        final AlertDialog d = b.create();
        final Runnable ok = new Runnable() { public void run() {
            if (done[0] || f.length() < minChars) return;
            done[0] = true; d.dismiss();
            nativeTextDone(token, true, f.getText().toString().getBytes(StandardCharsets.UTF_8));
        } };
        // Cancel, back, or the box gone any other way: a cancel, given once
        d.setOnDismissListener(new DialogInterface.OnDismissListener() { public void onDismiss(DialogInterface x) {
            if (done[0]) return;
            done[0] = true; nativeTextDone(token, false, null);
        } });
        f.setOnEditorActionListener(new TextView.OnEditorActionListener() { public boolean onEditorAction(TextView v, int action, KeyEvent e) {
            boolean enter = action == EditorInfo.IME_ACTION_DONE || (e != null && e.getKeyCode() == KeyEvent.KEYCODE_ENTER && e.getAction() == KeyEvent.ACTION_DOWN);
            if (enter) ok.run();
            return enter;
        } });
        d.getWindow().setSoftInputMode(WindowManager.LayoutParams.SOFT_INPUT_STATE_ALWAYS_VISIBLE);
        try { d.show(); }
        catch (WindowManager.BadTokenException e) {   // the activity's window went between the check above and here
            if (!done[0]) { done[0] = true; nativeTextDone(token, false, null); }
            return;
        }
        final Button okb = d.getButton(AlertDialog.BUTTON_POSITIVE);
        okb.setOnClickListener(new android.view.View.OnClickListener() { public void onClick(android.view.View v) { ok.run(); } });   // not the builder's: that one would close the box on a short text
        okb.setEnabled(f.length() >= minChars);
        f.addTextChangedListener(new TextWatcher() {
            public void beforeTextChanged(CharSequence s, int a, int c, int n) {}
            public void onTextChanged(CharSequence s, int a, int c, int n) {}
            public void afterTextChanged(Editable s) { okb.setEnabled(s.length() >= minChars); }
        });
        f.requestFocus();
    }
}
