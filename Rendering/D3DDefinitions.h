#pragma once

#include <d3d11.h>

#if DEBUG

const UINT DefaultD3D11DeviceFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_DEBUG;

#else

const UINT DefaultD3D11DeviceFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;

#endif