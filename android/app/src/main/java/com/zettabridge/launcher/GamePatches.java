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

    // Custom skins. "Choose New Skin" first asks for WRITE_EXTERNAL_STORAGE, because the game then
    // opens the picked image by its path, and the native answer handler
    // nativeStoragePermissionRequestResult(granted, reason) opens the picker only when granted.
    // From Android 11 on, this app cannot be granted that permission, so the button did nothing.
    // The launcher now hands the game a private copy of the picked image (ImagePickActivity), so
    // the handler's jump table sends reason 0 (custom skin) where reason 1 (import, which goes
    // ahead either way) goes. Reason 2, the world storage location, still needs the grant.
    private static final String SKIN_FUNCTION =
            "Java_com_mojang_minecraftpe_MainActivity_nativeStoragePermissionRequestResult";
    private static final byte[] SKIN_ORIGINAL = bytes(
            0x80, 0xb5,              // push {r7, lr}
            0x03, 0x2b,              // cmp r3, #3
            0x88, 0xbf, 0x80, 0xbd,  // it hi; pophi {r7, pc}
            0xdf, 0xe8, 0x03, 0xf0,  // tbb [pc, r3]
            0x02, 0x03, 0x0b, 0x02,  // reason 0 -> cbz, 1 -> blx, 2 -> storage location, 3 -> cbz
            0x92, 0xb1,              // cbz r2, <return>
            0xea, 0xf7, 0x96, 0xe0,  // blx AppPlatform::singleton
            0x01, 0x68,              // ldr r1, [r0]
            0xd1, 0xf8, 0xb8, 0x10,  // ldr.w r1, [r1, #0xb8]  runStoragePermissionResultCallback
            0xbd, 0xe8, 0x80, 0x40,  // pop.w {r7, lr}
            0x08, 0x47);             // bx r1
    private static final int SKIN_REASON_0 = 12;
    private static final byte SKIN_REASON_0_PATCHED = 0x03;

    private GamePatches() {}

    /** Applies the fixes to the imported game's libraries; one that does not apply is logged. */
    static void apply(File libDir) {
        File library = new File(libDir, LIBRARY);
        try {
            String result = patch(library, SKIN_FUNCTION, SKIN_ORIGINAL, SKIN_REASON_0, SKIN_REASON_0_PATCHED);
            Log.i(TAG, "custom skin picker: " + result);
        } catch (IOException | RuntimeException e) {
            Log.w(TAG, "custom skin picker: cannot patch " + library, e);
        }
    }

    /** Writes value at function + offset when the function starts with expected. */
    private static String patch(File library, String function, byte[] expected, int offset, byte value)
            throws IOException {
        // Imported libraries are read-only (PluginFiles.extractLibraries).
        if (!library.setWritable(true, true)) throw new IOException("cannot make " + library + " writable");
        try (RandomAccessFile file = new RandomAccessFile(library, "rw")) {
            MappedByteBuffer elf = file.getChannel().map(FileChannel.MapMode.READ_WRITE, 0, file.length());
            elf.order(ByteOrder.LITTLE_ENDIAN);
            int at = symbolOffset(elf, function);
            if (at < 0 || at + expected.length > elf.limit()) return function + " not found, unchanged";
            boolean original = true;
            boolean patched = true;
            for (int i = 0; i < expected.length; i++) {
                byte b = elf.get(at + i);
                original &= b == expected[i];
                patched &= b == (i == offset ? value : expected[i]);
            }
            if (patched) return "already patched";
            if (!original) return function + " is not the known build, unchanged";
            elf.put(at + offset, value);
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

    private static byte[] bytes(int... values) {
        byte[] out = new byte[values.length];
        for (int i = 0; i < values.length; i++) out[i] = (byte) values[i];
        return out;
    }
}
