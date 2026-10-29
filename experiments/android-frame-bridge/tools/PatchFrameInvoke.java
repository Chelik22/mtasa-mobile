package org.mta.mobile.tools;

import com.android.tools.smali.dexlib2.DexFileFactory;
import com.android.tools.smali.dexlib2.Opcode;
import com.android.tools.smali.dexlib2.Opcodes;
import com.android.tools.smali.dexlib2.iface.ClassDef;
import com.android.tools.smali.dexlib2.iface.DexFile;
import com.android.tools.smali.dexlib2.iface.Method;
import com.android.tools.smali.dexlib2.iface.MethodImplementation;
import com.android.tools.smali.dexlib2.iface.instruction.Instruction;
import com.android.tools.smali.dexlib2.iface.instruction.ReferenceInstruction;
import com.android.tools.smali.dexlib2.iface.instruction.formats.Instruction35c;
import com.android.tools.smali.dexlib2.iface.reference.MethodReference;
import com.android.tools.smali.dexlib2.immutable.ImmutableMethod;
import com.android.tools.smali.dexlib2.immutable.ImmutableMethodImplementation;
import com.android.tools.smali.dexlib2.immutable.instruction.ImmutableInstruction35c;
import com.android.tools.smali.dexlib2.immutable.reference.ImmutableMethodReference;
import com.android.tools.smali.dexlib2.rewriter.DexRewriter;
import com.android.tools.smali.dexlib2.rewriter.Rewriter;
import com.android.tools.smali.dexlib2.rewriter.RewriterModule;
import com.android.tools.smali.dexlib2.rewriter.Rewriters;
import com.android.tools.smali.dexlib2.writer.pool.DexPool;
import java.io.InputStream;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.Paths;
import java.security.MessageDigest;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.HashSet;
import java.util.List;
import java.util.Set;

/** Exact invoke-reference edit using the installed official SDK dexlib2 writer. */
public final class PatchFrameInvoke {
    private static final String EXECUTOR = "Lcom/rockstargames/oswrapper/GameThread$ExecutorThread;";
    private static final String GAME = "Lcom/rockstargames/oswrapper/GameNative;";
    private static final String OWN = "Lorg/mta/mobile/frame/FrameBridge;";
    private static final Set<String> PINNED_DEX = new HashSet<>(Arrays.asList(
            "6eb946d749c0b851235fe6bdbad05bdbd51529c7415780da886bcfed972fd937",
            "c4107a9a4c19525a92845b47c0cf1b02d15aafad974679355b2036f3e547a3f4",
            "6d5339876b501d6e1edf8b6cfd0e2ed904c76d50f953eab802795caff5d7d0c9"));

    private static String sha(Path path) throws Exception {
        MessageDigest hash = MessageDigest.getInstance("SHA-256");
        try (InputStream input = Files.newInputStream(path)) {
            byte[] buffer = new byte[32768];
            int count;
            while ((count = input.read(buffer)) != -1) hash.update(buffer, 0, count);
        }
        StringBuilder result = new StringBuilder();
        for (byte value : hash.digest()) result.append(String.format("%02x", value & 255));
        return result.toString();
    }

    private static boolean target(Method method) {
        return method.getDefiningClass().equals(EXECUTOR) && method.getName().equals("guardedRun")
                && method.getParameterTypes().isEmpty() && method.getReturnType().equals("V");
    }

    private static boolean draw(MethodReference method, boolean restore) {
        return method.getDefiningClass().equals(restore ? OWN : GAME)
                && method.getName().equals(restore ? "draw" : "implOnDrawFrame")
                && method.getReturnType().equals("V") && method.getParameterTypes().size() == 1
                && method.getParameterTypes().get(0).toString().equals("F");
    }

    private static DexFile transform(DexFile input, boolean restore) {
        int[] visited = {0};
        DexFile rewritten = new DexRewriter(new RewriterModule() {
            @Override public Rewriter<Method> getMethodRewriter(Rewriters rewriters) {
                return method -> {
                    if (!target(method)) return method;
                    visited[0]++;
                    MethodImplementation implementation = method.getImplementation();
                    if (implementation == null || implementation.getRegisterCount() != 7)
                        throw new IllegalStateException("pinned guardedRun implementation/register count changed");
                    List<Instruction> instructions = new ArrayList<>();
                    int replacements = 0;
                    for (Instruction instruction : implementation.getInstructions()) {
                        if (instruction instanceof ReferenceInstruction
                                && ((ReferenceInstruction) instruction).getReference() instanceof MethodReference
                                && draw((MethodReference) ((ReferenceInstruction) instruction).getReference(), restore)) {
                            if (instruction.getOpcode() != Opcode.INVOKE_STATIC || !(instruction instanceof Instruction35c))
                                throw new IllegalStateException("unexpected draw instruction format");
                            Instruction35c call = (Instruction35c) instruction;
                            if (call.getRegisterCount() != 1 || call.getRegisterC() != 4 || call.getCodeUnits() != 3)
                                throw new IllegalStateException("pinned draw registers/width changed");
                            instructions.add(new ImmutableInstruction35c(Opcode.INVOKE_STATIC, call.getRegisterCount(),
                                    call.getRegisterC(), call.getRegisterD(), call.getRegisterE(), call.getRegisterF(), call.getRegisterG(),
                                    new ImmutableMethodReference(restore ? GAME : OWN, restore ? "implOnDrawFrame" : "draw",
                                            Collections.singletonList("F"), "V")));
                            replacements++;
                        } else instructions.add(instruction);
                    }
                    if (replacements != 1) throw new IllegalStateException("expected exactly one draw reference");
                    return new ImmutableMethod(method.getDefiningClass(), method.getName(), method.getParameters(),
                            method.getReturnType(), method.getAccessFlags(), method.getAnnotations(), method.getHiddenApiRestrictions(),
                            new ImmutableMethodImplementation(implementation.getRegisterCount(), instructions,
                                    implementation.getTryBlocks(), implementation.getDebugItems()));
                };
            }
        }).getDexFileRewriter().rewrite(input);
        // Materialize exactly once: the rewriter is lazy, so counting callbacks
        // without writing is not evidence that the requested change occurred.
        return rewritten;
    }

    private static void validate(DexFile file, boolean patched) {
        int methods = 0, originalDeclaration = 0, calls = 0;
        for (ClassDef definition : file.getClasses()) {
            if (definition.getType().equals(OWN)) throw new IllegalStateException("own shim unexpectedly already in source DEX");
            for (Method method : definition.getMethods()) {
                if (definition.getType().equals(GAME) && method.getName().equals("implOnDrawFrame")) {
                    if (!draw(method, false) || (method.getAccessFlags() & 0x109) != 0x109 || method.getImplementation() != null)
                        throw new IllegalStateException("original public static native declaration changed");
                    originalDeclaration++;
                }
                if (!target(method)) continue;
                methods++;
                if (method.getImplementation() == null || method.getImplementation().getRegisterCount() != 7)
                    throw new IllegalStateException("guardedRun metadata changed");
                for (Instruction instruction : method.getImplementation().getInstructions()) {
                    if (instruction instanceof ReferenceInstruction
                            && ((ReferenceInstruction) instruction).getReference() instanceof MethodReference) {
                        MethodReference reference = (MethodReference) ((ReferenceInstruction) instruction).getReference();
                        if (draw(reference, !patched)) throw new IllegalStateException("old/unexpected draw target retained");
                        if (draw(reference, patched)) calls++;
                    }
                }
            }
        }
        if (methods != 1 || originalDeclaration != 1 || calls != 1)
            throw new IllegalStateException("exact method/native declaration/call counts not satisfied");
    }

    public static void main(String[] args) throws Exception {
        if (args.length != 2) throw new IllegalArgumentException("input-pinned.dex fresh-private-audit-directory");
        Path original = Paths.get(args[0]).toAbsolutePath().normalize();
        Path output = Paths.get(args[1]).toAbsolutePath().normalize();
        String originalSha = sha(original);
        if (!PINNED_DEX.contains(originalSha)) throw new IllegalArgumentException("source DEX fingerprint not pinned");
        if (Files.exists(output)) throw new IllegalArgumentException("audit directory must be fresh");
        Files.createDirectories(output);
        DexFile input = DexFileFactory.loadDexFile(original.toFile(), Opcodes.forApi(28));
        validate(input, false);
        Path canonical = output.resolve("canonical-original.dex");
        Path patched = output.resolve("patched.dex");
        Path restored = output.resolve("canonical-restored.dex");
        DexPool.writeTo(canonical.toString(), input);
        DexPool.writeTo(patched.toString(), transform(input, false));
        DexFile roundtrip = DexFileFactory.loadDexFile(patched.toFile(), Opcodes.forApi(28));
        validate(roundtrip, true);
        DexPool.writeTo(restored.toString(), transform(roundtrip, true));
        if (!Arrays.equals(Files.readAllBytes(canonical), Files.readAllBytes(restored)))
            throw new IllegalStateException("canonical restoration differs: metadata/instruction/control-flow change");
        if (!sha(original).equals(originalSha)) throw new IllegalStateException("original DEX changed");
        System.out.println("PASS exact invoke-only dexlib2 canonical roundtrip; original_sha256=" + originalSha
                + " patched_sha256=" + sha(patched) + " classes=" + input.getClasses().size()
                + "; helper insertion/APK/signing/install/game NOT RUN");
    }
}
