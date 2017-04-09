import tvm
from tvm.addon import testing, verilog
import numpy as np

def lower(s, args, name):
    binds = {}
    arg_list = []

    for x in args:
        assert isinstance(x, tvm.tensor.Tensor)
        buf = tvm.Buffer(x.shape, dtype=x.dtype, name=x.op.name)
        binds[x] = buf
        arg_list.append(buf)
    s.normalize()
    bounds = tvm.schedule.InferBound(s)
    stmt = tvm.schedule.ScheduleOps(s, bounds)
    stmt = tvm.ir_pass.StorageFlatten(stmt, binds)
    stmt = tvm.ir_pass.CanonicalSimplify(stmt)
    stmt = tvm.ir_pass.Simplify(stmt)
    stmt = tvm.ir_pass.SplitPipeline(stmt, True)
    fapi = tvm.ir_pass.MakeAPI(stmt, name, arg_list, 0)
    return fapi

@tvm.register_func
def tvm_callback_verilog_postproc(code):
    """Hook to inspect the verilog code before actually run it"""
    print(code)
    return code

def test_add_pipeline():
    nn = 128
    n = tvm.convert(nn)
    A = tvm.placeholder((n,), name='A', dtype='int32')
    B = tvm.placeholder((n,), name='B', dtype='int32')
    C = tvm.compute(A.shape, lambda i: A[i] + B[i], name='C')
    s = tvm.Schedule(C.op)

    px, x = s[C].split(C.op.axis[0], nparts=1)
    s[C].bind(px, tvm.thread_axis("pipeline"))
    fapi = lower(s, [A, B, C], "myadd")
    fsplits = tvm.ir_pass.SplitHostDevice(fapi)
    print(fsplits[1].body)
    print("------")

    def check_target(device, host="stackvm"):
        if not tvm.codegen.enabled(host):
            return
        if not tvm.codegen.enabled(device):
            return
        ctx = tvm.vpi(0)
        mhost = tvm.codegen.build(fsplits[0], host)
        mdev = tvm.codegen.build(fsplits[1:], device)
        mhost.import_module(mdev)
        code = mdev.get_source()
        f = mhost.entry_func
        # launch the kernel.
        n = nn
        a = tvm.nd.array((np.random.uniform(size=n) * 128).astype(A.dtype), ctx)
        b = tvm.nd.array((np.random.uniform(size=n) * 128).astype(A.dtype), ctx)
        c = tvm.nd.array(np.zeros(n, dtype=C.dtype), ctx)
        f(a, b, c)
        print("Check correctness...")
        np.testing.assert_allclose(
            c.asnumpy(), a.asnumpy() + b.asnumpy())
    check_target("verilog")


if __name__ == "__main__":
    test_add_pipeline()
