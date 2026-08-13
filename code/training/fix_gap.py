#!/usr/bin/env python3
"""把 ONNX 末尾的 ReduceMean(axes=[-1,-2],keepdims=1) 替换为 GlobalAveragePool。

原因: TPU-MLIR cv181x asymmetric INT8 下, ReduceMean 被下降为 AvgPool 触发
      AvgPool.cpp Assertion '!asymmetric'。GlobalAveragePool 有专门且
      asymmetric 友好的 cv18xx 处理路径, 可绕过。

用法: python3 fix_gap.py <in.onnx> <out.onnx>
"""
import sys
import onnx
from onnx import helper


def main():
    if len(sys.argv) < 3:
        print("用法: python3 fix_gap.py <in.onnx> <out.onnx>")
        return 1
    in_path, out_path = sys.argv[1], sys.argv[2]

    m = onnx.load(in_path)
    new_nodes = []
    replaced = 0
    for n in m.graph.node:
        if n.op_type == 'ReduceMean':
            axes = None
            keepdims = 1
            for a in n.attribute:
                if a.name == 'axes':
                    axes = list(a.ints)
                if a.name == 'keepdims':
                    keepdims = a.i
            # 检测是否为对最后两维(H,W)的全局平均, keepdims=1
            if axes in ([-1, -2], [-2, -1]) and keepdims == 1:
                gap = helper.make_node(
                    'GlobalAveragePool',
                    inputs=[n.input[0]],
                    outputs=list(n.output),
                    name=n.name + '_gap',
                )
                new_nodes.append(gap)
                replaced += 1
                print(f"替换 {n.name}: ReduceMean(axes={axes},keepdims=1) -> GlobalAveragePool")
            else:
                new_nodes.append(n)
        else:
            new_nodes.append(n)

    del m.graph.node[:]
    m.graph.node.extend(new_nodes)

    onnx.save_model(m, out_path)
    print(f"已保存: {out_path}, 替换 GlobalAveragePool x{replaced}")

    m2 = onnx.load(out_path)
    ops = {}
    for nn in m2.graph.node:
        ops[nn.op_type] = ops.get(nn.op_type, 0) + 1
    print("替换后算子统计:", ops)
    # 验证输出形状推断
    import onnx.shape_inference as si
    inferred = si.infer_shapes(m2)
    for vi in inferred.graph.value_info:
        if vi.name == 'mean':
            dims = [d.dim_value for d in vi.type.tensor_type.shape.dim]
            print("输出 'mean' 形状:", dims)
    return 0


if __name__ == '__main__':
    sys.exit(main())
