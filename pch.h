#pragma once

// Windows SDK
#include <unknwn.h>
#include <restrictederrorinfo.h>
#include <hstring.h>

// WinRT base
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>

// WinUI / XAML
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Controls.Primitives.h>
#include <winrt/Microsoft.UI.Xaml.Data.h>
#include <winrt/Microsoft.UI.Xaml.Input.h>
#include <winrt/Microsoft.UI.Xaml.Interop.h>
#include <winrt/Microsoft.UI.Xaml.Markup.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Microsoft.UI.Xaml.Shapes.h>
#include <winrt/Microsoft.UI.Xaml.Documents.h>
#include <winrt/Microsoft.UI.Xaml.Navigation.h>
#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Microsoft.UI.Interop.h>
#include <winrt/Microsoft.UI.Windowing.h>

// WinRT JSON
#include <winrt/Windows.Data.Json.h>

// Storage / Pickers
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.Storage.Pickers.h>

// Clipboard
#include <winrt/Windows.ApplicationModel.DataTransfer.h>

// Input (PointerPoint)
#include <winrt/Microsoft.UI.Input.h>
#include <winrt/Microsoft.UI.h>

// System (VirtualKey)
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.Text.h>

// Direct3D / Direct2D / DXGI
#include <d3d11_4.h>
#include <d2d1_3.h>
#include <d2d1_3helper.h>
#include <d2d1effects_2.h>
#include <d2d1effectauthor_1.h>
#include <dxgi1_6.h>
#include <dwrite_3.h>

// Shader compilation
#include <d3dcompiler.h>

// D2D effect authoring helpers (D2D1_VALUE_TYPE_BINDING macro)
#include <d2d1effecthelpers.h>

// WIC (image loading)
#include <wincodec.h>

// Media Foundation (video decoding)
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>

// Interop helpers
#include <microsoft.ui.xaml.window.h>
#include <Shobjidl.h>

// STL
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>
