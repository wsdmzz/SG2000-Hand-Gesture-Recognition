#!/usr/bin/env python3
"""修复 TPU-MLIR v1.3.228 的 SliceAxisOp 'none' operand 问题。

背景:
  TPU-MLIR 的 OnnxConverter.convert_slice_op 里，当 Slice 只有 4 个输入
  (data/starts/ends/axes, steps 省略) 且 ends 或 starts 非 const 时，会构造
  top.SliceAxisOp，其中 step_op 用 self.mlir.none_op 占位 -> MLIR 报
  "'top.SliceAxis' op operand #3 must be tensor of any type values, but got 'none'"

修复:
  给每个 4 输入的 Slice 补上第 5 个输入 steps = [1] 常量 (int64)。
  ONNX 规范中 steps 省略即默认 1，所以语义完全等价。

用法:
  python3 fix_onnx_slice_steps.py <in.onnx> <out.onnx>
"""
import sys
import numpy as np
import onnx
from onnx import helper, numpy_helper as nh


def main():
    if len(sys.argv) < 3:
        print("用法: python3 fix_onnx_slice_steps.py <in.onnx> <out.onnx>")
        sys.exit(1)
    in_path, out_path = sys.argv[1], sys.argv[2]

    m = onnx.load(in_path)

    used_names = {i.name for i in m.graph.initializer}
    used_names |= {o for n in m.graph.node for o in n.output}
    used_names |= {i for n in m.graph.node for i in n.input}

    fixed = 0
    for n in m.graph.node:
        if n.op_type != 'Slice' or len(n.input) != 4:
            continue
        # axes 用于决定 steps 长度; 取 axes 常量的元素个数
        axes_name = n.input[3]
        steps_len = 1
        for init in m.graph.initializer:
            if init.name == axes_name:
                steps_len = int(nh.to_array(init).size)
                break

        steps_name = f"{n.name}_steps_const"
        suffix = 0
        while steps_name in used_names:
            suffix += 1
            steps_name = f"{n.name}_steps_const_{suffix}"
        used_names.add(steps_name)

        arr = np.ones(steps_len, dtype=np.int64)
        m.graph.initializer.append(nh.from_array(arr, name=steps_name))
        n.input.append(steps_name)
        fixed += 1
        print(f"  已为 {n.name} 补 steps={arr.tolist()} (输入 -> {len(n.input)} 个)")

    print(f"共修复 {fixed} 个 Slice 节点")

    onnx.checker.check_model(m)
    onnx.save_model(m, out_path)
    print(f"已保存: {out_path}")

    # 复核
    m2 = onnx.load(out_path)
    c4 = [n.name for n in m2.graph.node if n.op_type == 'Slice' and len(n.input) == 4]
    print(f"复核: 仍为 4 输入的 Slice = {len(c4)} {c4}")
    for n in m2.graph.node:
        if n.op_type == 'Slice':
            print(f"  {n.name}: {len(n.input)} 输入")


if __name__ == '__main__':
    main()
