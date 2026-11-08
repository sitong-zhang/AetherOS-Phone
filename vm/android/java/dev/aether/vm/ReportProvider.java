package dev.aether.vm;

import android.content.ContentProvider;
import android.content.ContentValues;
import android.database.Cursor;
import android.database.MatrixCursor;
import android.net.Uri;
import android.os.ParcelFileDescriptor;
import android.provider.OpenableColumns;

import java.io.File;
import java.io.FileNotFoundException;

/**
 * 极简文件分享 Provider（自研，1.0.7）。
 *
 * 工程刻意不引入 androidx（1.0.1 因缺 androidx 的 AppCompatActivity 直接闪退），
 * 所以不能用 androidx.core.content.FileProvider。这里只做两件事：
 *   - openFile()：把 files/reports/ 下的报告 .txt 以 content:// 暴露给分享面板；
 *   - query()  ：返回 _display_name / _size，微信、Gmail 等目标 App 依赖它。
 *
 * exported=false + grantUriPermissions=true：只有本次 ACTION_SEND 被授权的目标
 * App 能临时读；路径做了文件名白名单，防止 ../ 穿越到私有目录之外。
 */
public final class ReportProvider extends ContentProvider {

    @Override
    public boolean onCreate() { return true; }

    @Override
    public ParcelFileDescriptor openFile(Uri uri, String mode)
            throws FileNotFoundException {
        return ParcelFileDescriptor.open(resolve(uri),
                ParcelFileDescriptor.MODE_READ_ONLY);
    }

    @Override
    public Cursor query(Uri uri, String[] projection, String selection,
                        String[] selectionArgs, String sortOrder) {
        File f;
        try { f = resolve(uri); } catch (FileNotFoundException e) { return null; }
        String[] cols = projection != null ? projection : new String[]{
                OpenableColumns.DISPLAY_NAME, OpenableColumns.SIZE};
        MatrixCursor c = new MatrixCursor(cols, 1);
        Object[] row = new Object[cols.length];
        for (int i = 0; i < cols.length; i++) {
            if (OpenableColumns.DISPLAY_NAME.equals(cols[i])) row[i] = f.getName();
            else if (OpenableColumns.SIZE.equals(cols[i]))     row[i] = f.length();
            else                                               row[i] = null;
        }
        c.addRow(row);
        return c;
    }

    @Override public String getType(Uri u) { return "text/plain"; }
    @Override public Uri insert(Uri u, ContentValues v) { return null; }
    @Override public int delete(Uri u, String s, String[] a) { return 0; }
    @Override public int update(Uri u, ContentValues v, String s, String[] a) { return 0; }

    /** 只接受 files/reports/ 下的纯 .txt 文件名，杜绝路径穿越 */
    private File resolve(Uri uri) throws FileNotFoundException {
        String name = uri.getLastPathSegment();
        if (name == null || name.indexOf('/') >= 0 || name.indexOf('\\') >= 0
                || name.contains("..") || !name.endsWith(".txt"))
            throw new FileNotFoundException("非法报告文件名");
        File dir = new File(getContext().getFilesDir(), "reports");
        File f = new File(dir, name);
        if (!f.isFile()) throw new FileNotFoundException(name);
        return f;
    }
}
