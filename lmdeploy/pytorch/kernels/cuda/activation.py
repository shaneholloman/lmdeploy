# Copyright (c) OpenMMLab. All rights reserved.
import torch
import triton
import triton.language as tl
from packaging import version

from .utils import get_device_props

TRITON_VERSION = version.parse(triton.__version__)

if TRITON_VERSION >= version.parse('3.0.0'):
    fast_expf = tl.math.exp
else:
    fast_expf = tl.math.fast_expf


@triton.jit
def _silu_and_mul_kernel(
    gateup_ptr,
    out_ptr,
    N: tl.constexpr,
    M,
    stride_gum: tl.constexpr,
    stride_gun: tl.constexpr,
    stride_om: tl.constexpr,
    stride_on: tl.constexpr,
    BLOCK_SIZE_N: tl.constexpr,
):
    """Silu and mul kernel."""
    n_block_id = tl.program_id(0)
    m_id_start = tl.program_id(1)
    m_id_stride = tl.num_programs(1)

    up_ptr = gateup_ptr + N * stride_gun
    offs_n = n_block_id * BLOCK_SIZE_N + tl.arange(0, BLOCK_SIZE_N)

    if N % BLOCK_SIZE_N == 0:
        mask = None
    else:
        mask = offs_n < N

    gate_ptrs = gateup_ptr + m_id_start * stride_gum + offs_n * stride_gun
    up_ptrs = up_ptr + m_id_start * stride_gum + offs_n * stride_gun
    out_ptrs = out_ptr + m_id_start * stride_om + offs_n * stride_on

    for _ in tl.range(m_id_start, M, m_id_stride):
        gate = tl.load(gate_ptrs, mask=mask)
        up = tl.load(up_ptrs, mask=mask)
        gate = gate.to(tl.float32)
        up = up.to(tl.float32)

        gate = gate / (1 + fast_expf(-gate))
        out = gate * up

        tl.store(out_ptrs, out, mask=mask)

        gate_ptrs += m_id_stride * stride_gum
        up_ptrs += m_id_stride * stride_gum
        out_ptrs += m_id_stride * stride_om


def silu_and_mul(gate_up: torch.Tensor, out: torch.Tensor = None):
    """Silu and mul."""
    assert gate_up.dim() == 2

    M = gate_up.size(0)
    N = gate_up.size(-1) // 2
    if out is None:
        out_shape = (M, N)
        out = gate_up.new_empty(out_shape)

    BLOCK_SIZE_N = triton.next_power_of_2(N)
    BLOCK_SIZE_N = min(BLOCK_SIZE_N, 512)
    num_warps = 4
    num_stages = 1

    props = get_device_props(gate_up.device.index)
    num_sm = props['multi_processor_count']
    warps_per_sm = props['warps_per_sm']
    grid_size0 = triton.cdiv(N, BLOCK_SIZE_N)
    grid_size1 = min(M, num_sm * warps_per_sm // num_warps)
    assert grid_size0 < 65536 and grid_size1 < 65536
    grid = (grid_size0, grid_size1)
    _silu_and_mul_kernel[grid](gate_up,
                               out,
                               N,
                               M,
                               stride_gum=gate_up.stride(0),
                               stride_gun=gate_up.stride(1),
                               stride_om=out.stride(0),
                               stride_on=out.stride(1),
                               BLOCK_SIZE_N=BLOCK_SIZE_N,
                               num_warps=num_warps,
                               num_stages=num_stages)

    return out
