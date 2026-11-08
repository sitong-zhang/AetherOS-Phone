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
 * 更新包分享 Provider（自研，不依赖 androidx）。
 *
 * 把私有目录下的 AetherVM-update.apk 以 content:// 暴露给系统安装器。
 * Android 7.0 起禁止在 Intent 里传递 file:// URI，必须由 Provider 授予
 * 临时读权限；exported=false + grantUriPermissions=true 保证只有本次
 * ACTION_VIEW 被授权的安装器能读取。文件名白名单固定为单个常量，杜绝
 * 路径穿越到私有目录之外。
 */
public final class UpdateProvider extends ContentProvider {

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

    @Override public String getType(Uri u) {
        return "application/vnd.android.package-archive";
    }
    @Override public Uri insert(Uri u, ContentValues v) { return null; }
    @Override public int delete(Uri u, String s, String[] a) { return 0; }
    @Override public int update(Uri u, ContentValues v, String s, String[] a) { return 0; }

    private File resolve(Uri uri) throws FileNotFoundException {
        String name = uri.getLastPathSegment();
        if (name == null || !UpdateManager.APK_FILE.equals(name))
            throw new FileNotFoundException("非法的更新包文件名");
        File dir = UpdateManager.updateDir(getContext());
        File f = new File(dir, name);
        if (!f.isFile()) throw new FileNotFoundException(name);
        return f;
    }
}
