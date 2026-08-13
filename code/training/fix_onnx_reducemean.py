#!/usr/bin/env python3
"""修复 ONNX 模型以便 TPU-MLIR v1.28.1 转换。

处理两个 ONNX 规范问题（PyTorch/Keras 导出时常见）：
1. ReduceMean 的 axes 从"输入张量"(opset>=18) 改为"节点属性"(opset<=13 旧格式)
   - 原因: TPU-MLIR v1.28.1 对 axes 作为输入张量的 ReduceMean 报 "Unsupport opset for ReduceMean"
2. 给所有缺 kernel_shape 的 Conv 节点补齐 kernel_shape 属性
   - 原因: ONNX 规范允许缺省 kernel_shape（从权重推断），但 TPU-MLIR 要求显式 kernel_shape
   - 推导: 权重形状 [M, C/group, kH, kW] -> kernel_shape=[kH, kW]

用法:
    python3 fix_onnx_reducemean.py <in.onnx> <out.onnx>
"""
import sys
import numpy as np
import onnx
from onnx import helper
from onnx import numpy_helper as nh


def fix_conv_kernel_shape(m):
    """给所有缺 kernel_shape 的 Conv 节点补齐，从权重张量推断。"""
    init_shape = {}
    for init in m.graph.initializer:
        init_shape[init.name] = nh.to_array(init).shape

    fixed_count = 0
    for n in m.graph.node:
        if n.op_type != 'Conv':
            continue
        has_ks = any(a.name == 'kernel_shape' for a in n.attribute)
        if has_ks:
            continue
        # 权重是第 2 个输入 (inputs[1])
        if len(n.input) < 2 or n.input[1] not in init_shape:
            print(f"  WARN 无法为 {n.name} 推断 kernel_shape: 缺权重或权重未初始化")
            continue
        w_shape = init_shape[n.input[1]]
        if len(w_shape) >= 4:
            kH, kW = w_shape[2], w_shape[3]
            n.attribute.append(helper.make_attribute('kernel_shape', [kH, kW]))
            fixed_count += 1
            print(f"  已为 {n.name} 补 kernel_shape=[{kH},{kW}] (权重{w_shape})")
        else:
            print(f"  WARN {n.name} 权重形状异常: {w_shape}")
    print(f"Conv kernel_shape 补齐: {fixed_count} 个节点")
    return fixed_count


def fix_reducemean_axes(m):
    """把 ReduceMean 的 axes 从输入张量改为属性。"""
    axes_init = {}
    for init in m.graph.initializer:
        if init.data_type != onnx.TensorProto.INT64:
            continue
        arr = np.frombuffer(init.raw_data, dtype=np.int64)
        axes_init[init.name] = arr.copy()

    new_nodes = []
    removed_init_names = set()
    used_input_names = set()
    for n in m.graph.node:
        for inp in n.input:
            used_input_names.add(inp)

    for n in m.graph.node:
        if n.op_type == 'ReduceMean' and len(n.input) >= 2 and n.input[1] in axes_init:
            data_in = n.input[0]
            axes_val = axes_init[n.input[1]]
            removed_init_names.add(n.input[1])

            attrs = {a.name: a for a in n.attribute}
            keepdims = attrs['keepdims'].i if 'keepdims' in attrs else 1

            new_node = helper.make_node(
                'ReduceMean',
                inputs=[data_in],
                outputs=list(n.output),
                name=n.name,
                axes=axes_val.tolist(),
                keepdims=keepdims,
            )
            new_nodes.append(new_node)
            print(f"已修复 {n.name}: axes={axes_val.tolist()} (从输入张量 {n.input[1]} 改为属性)")
        else:
            new_nodes.append(n)

    del m.graph.node[:]
    m.graph.node.extend(new_nodes)

    kept_init = []
    for init in m.graph.initializer:
        if init.name in removed_init_names and init.name not in used_input_names:
            continue
        kept_init.append(init)
    del m.graph.initializer[:]
    m.graph.initializer.extend(kept_init)
    return removed_init_names


def main():
    if len(sys.argv) < 3:
        print("用法: python3 fix_onnx_reducemean.py <in.onnx> <out.onnx>")
        sys.exit(1)
    in_path, out_path = sys.argv[1], sys.argv[2]

    m = onnx.load(in_path)

    print("=== 1. 修复 Conv kernel_shape ===")
    fix_conv_kernel_shape(m)

    print("=== 2. 修复 ReduceMean axes ===")
    fix_reducemean_axes(m)

    onnx.save_model(m, out_path)
    print(f"已保存: {out_path}")

    # 验证
    m2 = onnx.load(out_path)
    miss = [(n.name, n.op_type) for n in m2.graph.node
            if n.op_type == 'Conv' and not any(a.name == 'kernel_shape' for a in n.attribute)]
    print(f"验证: 仍缺 kernel_shape 的 Conv = {len(miss)} {miss[:5]}")
    nx = [n for n in m2.graph.node if n.op_type == 'ReduceMean']
    for n in nx:
        print(f"验证 ReduceMean {n.name}: inputs={list(n.input)}, axes_attr={[list(a.ints) for a in n.attribute if a.name=='axes']}")
    print("验证加载 OK")


if __name__ == '__main__':
    main()
