from pathlib import Path
import sys,tempfile,unittest,subprocess
from unittest.mock import patch
from contextlib import ExitStack
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
import vpi_build as builder
from vpi_provenance import digest,verify

class BuildFailureTests(unittest.TestCase):
    def exercise(self,mode):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder);out=root/'out';out.mkdir()
            source=root/'dut.sv';source.write_text('module dut; endmodule')
            compiler=root/'compiler';compiler.write_bytes(b'compiler')
            for name in ('tests/vpi_generic_main.cpp','src/minisim.cpp','tools/vpi_build.py','tools/pyslang_lower.py','tools/vpi_manifest.py','tools/vpi_provenance.py','tools/vpi_runtime.py'):
                p=root/name;p.parent.mkdir(parents=True,exist_ok=True);p.write_text(name)
            binary=out/'simulator';binary.write_bytes(b'previous working binary')
            model={'source_sha256':digest(source)}
            def compile_attempt(*args,**kwargs):
                if mode=='compiler_failure':raise subprocess.CalledProcessError(1,args[0])
                if mode=='compiler_timeout':raise subprocess.TimeoutExpired(args[0],120)
                (out/'simulator.building').write_bytes(b'new binary')
                if mode == 'concurrent':
                    with patch.object(builder, 'Lowerer') as competing_lowerer:
                        with self.assertRaisesRegex(RuntimeError, 'VPI build already in progress'):
                            builder.build(source, out)
                        competing_lowerer.assert_not_called()
                    self.assertEqual((out/'simulator.building').read_bytes(), b'new binary')
                if mode=='source_change':source.write_text('changed during compile')
                if mode=='header_change':(out/'design.hpp').write_text('changed generated header')
                return subprocess.CompletedProcess(args[0],0)
            def manifest_attempt(*args):
                if mode=='precompile_change':source.write_text('changed after lowering')
                return model
            def runtime_identity():
                if mode=='identity_failure':raise RuntimeError('runtime identity unavailable')
                if mode=='identity_source_change':source.write_text('changed during runtime discovery')
                return {}
            with ExitStack() as stack:
                stack.enter_context(patch.object(builder,'ROOT',root))
                lower=stack.enter_context(patch.object(builder,'Lowerer'));lower.return_value.lower.return_value=model
                stack.enter_context(patch.object(builder,'build_manifest',side_effect=manifest_attempt))
                stack.enter_context(patch.object(builder,'cpp_header',return_value='generated design'))
                stack.enter_context(patch.object(builder,'manifest_header',return_value='generated manifest'))
                stack.enter_context(patch.object(builder,'libraries',return_value=[]))
                stack.enter_context(patch.object(builder,'identity',side_effect=runtime_identity))
                stack.enter_context(patch.object(builder.shutil,'which',return_value=str(compiler)))
                compiler_call=stack.enter_context(patch.object(builder.subprocess,'run',side_effect=compile_attempt))
                if mode=='compiler_failure':
                    with self.assertRaises(subprocess.CalledProcessError):builder.build(source,out)
                elif mode=='compiler_timeout':
                    with self.assertRaises(subprocess.TimeoutExpired):builder.build(source,out)
                elif mode=='identity_failure':
                    with self.assertRaisesRegex(RuntimeError,'runtime identity unavailable'):builder.build(source,out)
                elif mode=='identity_source_change':
                    with self.assertRaisesRegex(RuntimeError,'input changed during runtime discovery'):builder.build(source,out)
                elif mode in ('source_change','header_change'):
                    with self.assertRaisesRegex(RuntimeError,'input changed'):builder.build(source,out)
                elif mode=='precompile_change':
                    with self.assertRaisesRegex(RuntimeError,'after lowering'):builder.build(source,out)
                    compiler_call.assert_not_called()
                else:
                    self.assertEqual(builder.build(source,out),binary)
                    verify(out/'provenance.json')
                    if mode == 'concurrent':
                        self.assertEqual(builder.build(source, out), binary)
                        verify(out/'provenance.json')
                        self.assertEqual(compiler_call.call_count, 2)
            if mode not in ('success', 'concurrent'):
                self.assertEqual(binary.read_bytes(),b'previous working binary')
                self.assertFalse((out/'provenance.json').exists())
            else:self.assertEqual(binary.read_bytes(),b'new binary')
    def test_compiler_failure_preserves_binary(self):self.exercise('compiler_failure')
    def test_compiler_timeout_preserves_binary(self):self.exercise('compiler_timeout')
    def test_changed_header_preserves_binary(self):self.exercise('header_change')
    def test_changed_input_preserves_binary(self):self.exercise('source_change')
    def test_changed_after_lowering_preserves_binary(self):self.exercise('precompile_change')
    def test_runtime_identity_failure_preserves_binary(self):self.exercise('identity_failure')
    def test_runtime_discovery_input_change_preserves_binary(self):self.exercise('identity_source_change')
    def test_success_replaces_and_records_binary(self):self.exercise('success')

    def test_competing_build_cannot_enter_output(self):
        self.exercise('concurrent')

if __name__=='__main__':unittest.main()
