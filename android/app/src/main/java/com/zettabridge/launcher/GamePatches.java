package com.zettabridge.launcher;

import android.util.Log;

import java.io.File;
import java.io.IOException;
import java.io.RandomAccessFile;
import java.nio.ByteOrder;
import java.nio.MappedByteBuffer;
import java.nio.channels.FileChannel;
import java.nio.charset.StandardCharsets;

/**
 * This game's own fixes to libminecraftpe.so, applied after every import of the bundled APK. A
 * fix names a function by its dynamic symbol and changes it only when the function's bytes are
 * exactly the ones it was written against; otherwise the library stays as it is and the game
 * behaves as it did before.
 */
final class GamePatches {
    private static final String TAG = "mcpe-arm64";
    private static final String LIBRARY = "libminecraftpe.so";

    // Custom skins. "Choose New Skin" first asks for storage permission, because the game then
    // opened the picked image by its path, and picks only once the permission is granted. The
    // launcher hands the game a private copy of the image (ImagePickActivity), so the permission
    // is not needed; and asking for it would now open the "All files access" settings page
    // (StorageAccessActivity) on every pick. So MinecraftScreenModel::pickCustomSkin queues the
    // pick on the main thread directly: AppPlatform::queueForMainThread (vtable 0x120) in place of
    // openStoragePermissionRequest (vtable 0xa0), with the same std::function. The storage
    // location and imports still ask.
    private static final String SKIN_FUNCTION =
            "_ZNK20MinecraftScreenModel14pickCustomSkinESt8functionIFvN14SkinRepository20PickCustomSkinResultEEE";
    // From the function's start through the call:
    //   +0x00 push.w {r4-r9, lr}; ...; blx AppPlatform::singleton; mov r4, r0
    //   +0x16 ldr.w r8, [r0, #0xa0]      openStoragePermissionRequest(reason, std::function<void()>)
    //   ...   builds the std::function at sp+0x14
    //   +0x66 add r2, sp, #0x14; mov r0, r4
    //   +0x6a movs r1, #0                 reason 0, custom skin
    //   +0x6c blx r8
    private static final byte[] SKIN_ORIGINAL = hex(
            "2de9f04389b00e4605464ef752e40446"
            + "4ff000092068d0f8a080cdf80890b368"
            + "43b16846314602229847f7680397b668"
            + "029600e00026ed680495cdf81c901420"
            + "4ef782e3dff88cc0dde90031224afc44"
            + "c0e90031002102917a44c0e902670561"
            + "0590cde9072c05aa20460021c047");
    private static final Edit[] SKIN_EDITS = {
        new Edit(0x16, hex("d0f82081")),  // ldr.w r8, [r0, #0x120]   queueForMainThread(std::function<void()>)
        new Edit(0x6a, hex("1146")),      // mov r1, r2                 the std::function
    };

    private GamePatches() {}

    /** Bytes written at an offset into a function. */
    private static final class Edit {
        final int offset;
        final byte[] bytes;

        Edit(int offset, byte[] bytes) {
            this.offset = offset;
            this.bytes = bytes;
        }
    }

    /** Applies the fixes to the imported game's libraries; one that does not apply is logged. */
    static void apply(File libDir) {
        File library = new File(libDir, LIBRARY);
        try {
            String result = patch(library, SKIN_FUNCTION, SKIN_ORIGINAL, SKIN_EDITS);
            Log.i(TAG, "custom skin picker: " + result);
        } catch (IOException | RuntimeException e) {
            Log.w(TAG, "custom skin picker: cannot patch " + library, e);
        }
    }

    /** Applies edits to a function when its bytes start with expected. */
    private static String patch(File library, String function, byte[] expected, Edit[] edits) throws IOException {
        byte[] patched = expected.clone();
        for (Edit edit : edits) System.arraycopy(edit.bytes, 0, patched, edit.offset, edit.bytes.length);
        // Imported libraries are read-only (PluginFiles.extractLibraries).
        if (!library.setWritable(true, true)) throw new IOException("cannot make " + library + " writable");
        try (RandomAccessFile file = new RandomAccessFile(library, "rw")) {
            MappedByteBuffer elf = file.getChannel().map(FileChannel.MapMode.READ_WRITE, 0, file.length());
            elf.order(ByteOrder.LITTLE_ENDIAN);
            int at = symbolOffset(elf, function);
            if (at < 0 || at + expected.length > elf.limit()) return function + " not found, unchanged";
            boolean isOriginal = true;
            boolean isPatched = true;
            for (int i = 0; i < expected.length; i++) {
                byte b = elf.get(at + i);
                isOriginal &= b == expected[i];
                isPatched &= b == patched[i];
            }
            if (isPatched) return "already patched";
            if (!isOriginal) return function + " is not the known build, unchanged";
            for (int i = 0; i < patched.length; i++) elf.put(at + i, patched[i]);
            elf.force();
            return "patched";
        } finally {
            if (!library.setReadOnly()) Log.w(TAG, "cannot make " + library + " read-only again");
        }
    }

    /** File offset of a dynamic symbol's code in an ELF32 little-endian library, or -1. */
    private static int symbolOffset(MappedByteBuffer elf, String name) {
        if (elf.limit() < 52 || elf.getInt(0) != 0x464c457f || elf.get(4) != 1 || elf.get(5) != 1) return -1;
        int dynamic = -1;
        int dynamicEnd = -1;
        for (int i = 0, phnum = elf.getShort(44) & 0xffff; i < phnum; i++) {
            int ph = elf.getInt(28) + i * 32;
            if (elf.getInt(ph) == 2) {  // PT_DYNAMIC
                dynamic = elf.getInt(ph + 4);
                dynamicEnd = dynamic + elf.getInt(ph + 16);
            }
        }
        int hash = -1;
        int strtab = -1;
        int symtab = -1;
        for (int d = dynamic; d >= 0 && d + 8 <= dynamicEnd && elf.getInt(d) != 0; d += 8) {
            int tag = elf.getInt(d);
            if (tag == 4) hash = fileOffset(elf, elf.getInt(d + 4));         // DT_HASH
            else if (tag == 5) strtab = fileOffset(elf, elf.getInt(d + 4));  // DT_STRTAB
            else if (tag == 6) symtab = fileOffset(elf, elf.getInt(d + 4));  // DT_SYMTAB
        }
        if (hash < 0 || strtab < 0 || symtab < 0) return -1;
        byte[] wanted = name.getBytes(StandardCharsets.US_ASCII);
        int buckets = elf.getInt(hash);
        int chains = hash + 8 + 4 * buckets;
        for (int i = elf.getInt(hash + 8 + 4 * (int) (elfHash(wanted) % buckets)); i != 0; i = elf.getInt(chains + 4 * i)) {
            int symbol = symtab + 16 * i;
            if (nameEquals(elf, strtab + elf.getInt(symbol), wanted)) {
                return fileOffset(elf, elf.getInt(symbol + 4) & ~1);  // Thumb bit
            }
        }
        return -1;
    }

    /** The file offset of a virtual address inside a PT_LOAD segment, or -1. */
    private static int fileOffset(MappedByteBuffer elf, int address) {
        for (int i = 0, phnum = elf.getShort(44) & 0xffff; i < phnum; i++) {
            int ph = elf.getInt(28) + i * 32;
            int vaddr = elf.getInt(ph + 8);
            if (elf.getInt(ph) == 1 && address - vaddr >= 0 && address - vaddr < elf.getInt(ph + 16)) {
                return elf.getInt(ph + 4) + (address - vaddr);
            }
        }
        return -1;
    }

    private static long elfHash(byte[] name) {
        long h = 0;
        for (byte b : name) {
            h = (h << 4) + (b & 0xff);
            long g = h & 0xf0000000L;
            if (g != 0) h ^= g >>> 24;
            h &= ~g;
        }
        return h;
    }

    private static boolean nameEquals(MappedByteBuffer elf, int at, byte[] wanted) {
        if (at < 0 || at + wanted.length >= elf.limit()) return false;
        for (int i = 0; i < wanted.length; i++) {
            if (elf.get(at + i) != wanted[i]) return false;
        }
        return elf.get(at + wanted.length) == 0;
    }

    private static byte[] hex(String digits) {
        byte[] out = new byte[digits.length() / 2];
        for (int i = 0; i < out.length; i++) out[i] = (byte) Integer.parseInt(digits.substring(2 * i, 2 * i + 2), 16);
        return out;
    }
}
