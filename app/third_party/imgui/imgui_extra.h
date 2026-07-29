#pragma once
#include "pch.h"

#include "resources/icons.hpp"
#include "menu/render/texture.hpp"
#include "ui/imgui_widget_integration.hpp"
#include "game/globals.hpp"
#include "menu/window.hpp"
#include "menu/widgets.hpp"
#include "features/misc/helpers.hpp"
#include "app/config/config_manager.hpp"

// Key mapping for Hotkey function
const std::map<int, const char*> kKeyNamePairs = {
    {0x0,  "None"         },
    {0x01, "Left Mouse"   },
    {0x02, "Right Mouse"  },
    {0x04, "Middle Mouse" },
    {0x05, "Mouse 4"      },
    {0x06, "Mouse 5"      },
    {0x08, "Backspace"    },
    {0x09, "Tab"          },
    {0x0C, "Clear"        },
    {0x0D, "Enter"        },
    {0x10, "Shift"        },
    {0x11, "Control"      },
    {0x12, "Alt"          },
    {0x13, "Pause"        },
    {0x14, "Caps Lock"    },
    {0x1B, "Escape"       },
    {0x20, "Space"        },
    {0x21, "Page Up"      },
    {0x22, "Page Down"    },
    {0x23, "End"          },
    {0x24, "Home"         },
    {0x25, "Left"         },
    {0x26, "Up"           },
    {0x27, "Right"        },
    {0x28, "Down"         },
    {0x2D, "Insert"       },
    {0x2E, "Delete"       },
    {0x30, "0"            },
    {0x31, "1"            },
    {0x32, "2"            },
    {0x33, "3"            },
    {0x34, "4"            },
    {0x35, "5"            },
    {0x36, "6"            },
    {0x37, "7"            },
    {0x38, "8"            },
    {0x39, "9"            },
    {0x41, "A"            },
    {0x42, "B"            },
    {0x43, "C"            },
    {0x44, "D"            },
    {0x45, "E"            },
    {0x46, "F"            },
    {0x47, "G"            },
    {0x48, "H"            },
    {0x49, "I"            },
    {0x4A, "J"            },
    {0x4B, "K"            },
    {0x4C, "L"            },
    {0x4D, "M"            },
    {0x4E, "N"            },
    {0x4F, "O"            },
    {0x50, "P"            },
    {0x51, "Q"            },
    {0x52, "R"            },
    {0x53, "S"            },
    {0x54, "T"            },
    {0x55, "U"            },
    {0x56, "V"            },
    {0x57, "W"            },
    {0x58, "X"            },
    {0x59, "Y"            },
    {0x5A, "Z"            },
    {0x5B, "Left Windows" },
    {0x5C, "Right Windows"},
    {0x60, "Numpad 0"     },
    {0x61, "Numpad 1"     },
    {0x62, "Numpad 2"     },
    {0x63, "Numpad 3"     },
    {0x64, "Numpad 4"     },
    {0x65, "Numpad 5"     },
    {0x66, "Numpad 6"     },
    {0x67, "Numpad 7"     },
    {0x68, "Numpad 8"     },
    {0x69, "Numpad 9"     },
    {0x6A, "Multiply"     },
    {0x6B, "Add"          },
    {0x6C, "Separator"    },
    {0x6D, "Subtract"     },
    {0x6E, "Decimal"      },
    {0x6F, "Divide"       },
    {0x70, "F1"           },
    {0x71, "F2"           },
    {0x72, "F3"           },
    {0x73, "F4"           },
    {0x74, "F5"           },
    {0x75, "F6"           },
    {0x76, "F7"           },
    {0x77, "F8"           },
    {0x78, "F9"           },
    {0x79, "F10"          },
    {0x7A, "F11"          },
    {0x7B, "F12"          },
    {0x90, "Num Lock"     },
    {0x91, "Scroll Lock"  },
    {0xA0, "Left Shift"   },
    {0xA1, "Right Shift"  },
    {0xA2, "Left Control" },
    {0xA3, "Right Control"},
    {0xA4, "Left Menu"    },
    {0xA5, "Right Menu"   }
};

// Build lookup map and code list once at start-up – avoids duplicated manual lists
const std::map<int, const char*> KeyNames( std::begin( kKeyNamePairs ), std::end( kKeyNamePairs ) );

const std::vector<int> KeyCodes = [] {
  std::vector<int> v;
  v.reserve( std::size( kKeyNamePairs ) );
  for ( const auto& p : kKeyNamePairs )
    v.push_back( p.first );
  return v;
}();

inline ImVec4 copiedColor;
inline bool   hasColorCopied = false;

inline ID3D11ShaderResourceView* icon_copy_tex = nullptr;
inline ID3D11ShaderResourceView* icon_paste_tex = nullptr;

inline void DrawIconButton( const char* id, void* tex, const ImVec2& btnSize, bool& clicked, bool active_state ) {
  ImVec2      p       = ImGui::GetCursorScreenPos();
  clicked             = ImGui::InvisibleButton( id, btnSize );
  bool        hovered = ImGui::IsItemHovered();
  bool        active  = ImGui::IsItemActive();
  ImDrawList* draw    = ImGui::GetWindowDrawList();

  ImVec4 targetColor = colors::textDim;
  if ( active )
    targetColor = ImVec4( 1.0f, 1.0f, 1.0f, 1.0f );  // White on click (Druck)
  else if ( hovered )
    targetColor = ImVec4( 0.8f, 0.8f, 0.8f, 1.0f );  // Light grey on hover
  else if ( active_state )
    targetColor = ImVec4( 0.66f, 0.66f, 0.66f, 1.0f ); // Subtle highlight for "available" state

  const ImVec4 currentColor = WindowMotion::System().color(
      ui::motion::MotionKey( "icon_btn", id, "color" ),
      targetColor,
      WindowMotion::Tokens().hoverSoft,
      colors::textDim );

 ImU32 col    = ImGui::GetColorU32( currentColor );
  ImU32 shadow = ImGui::GetColorU32( ImVec4( currentColor.x, currentColor.y, currentColor.z, currentColor.w * 0.55f ) );

  float  iconSz = 20.0f;
  ImVec2 center = ImVec2( p.x + btnSize.x * 0.5f, p.y + btnSize.y * 0.5f );
  ImVec2 p1     = ImVec2( center.x - iconSz * 0.5f, center.y - iconSz * 0.5f );
  ImVec2 p2     = ImVec2( center.x + iconSz * 0.5f, center.y + iconSz * 0.5f );
  float  off    = 0.6f;

  if ( tex ) {
    draw->AddImage( (ImTextureID)tex, ImVec2( p1.x - off, p1.y ), ImVec2( p2.x - off, p2.y ), ImVec2( 0, 0 ), ImVec2( 1, 1 ), shadow );
    draw->AddImage( (ImTextureID)tex, ImVec2( p1.x + off, p1.y ), ImVec2( p2.x + off, p2.y ), ImVec2( 0, 0 ), ImVec2( 1, 1 ), shadow );
    draw->AddImage( (ImTextureID)tex, ImVec2( p1.x, p1.y - off ), ImVec2( p2.x, p2.y - off ), ImVec2( 0, 0 ), ImVec2( 1, 1 ), shadow );
    draw->AddImage( (ImTextureID)tex, ImVec2( p1.x, p1.y + off ), ImVec2( p2.x, p2.y + off ), ImVec2( 0, 0 ), ImVec2( 1, 1 ), shadow );
    draw->AddImage( (ImTextureID)tex, p1, p2, ImVec2( 0, 0 ), ImVec2( 1, 1 ), col );
  }
}

// Helper function to create a slightly lighter color
inline ImVec4 LightenColor( const ImVec4& color, float factor = 0.2f ) {
  return ImVec4(
      ImMin( color.x + factor, 1.0f ),
      ImMin( color.y + factor, 1.0f ),
      ImMin( color.z + factor, 1.0f ),
      color.w );
}

enum class ColorValueFormat {
  Hex,
  Rgb,
  Hsv,
  Hsl
};

inline constexpr int kVisiblePaletteCapacity = 5;

// Deliberately session-only. This is shared by every picker, defaults to HEX
// at process start, and is never registered with the config system.
inline ColorValueFormat colorValueFormat = ColorValueFormat::Hex;

inline float GetAdvancedColorPickerWidth() {
  return ImClamp( ImGui::GetFontSize() * 17.0f, 210.0f, 240.0f );
}

inline const char* GetColorValueFormatName( ColorValueFormat format ) {
  switch ( format ) {
    case ColorValueFormat::Rgb: return "RGB";
    case ColorValueFormat::Hsv: return "HSV";
    case ColorValueFormat::Hsl: return "HSL";
    default: return "HEX";
  }
}

inline int ColorByte( float value ) {
  return static_cast<int>( ImClamp( floorf( value * 255.0f + 0.5f ), 0.0f, 255.0f ) );
}

inline void ColorConvertRGBtoHSL( float r, float g, float b, float& h, float& s, float& l ) {
  const float maxChannel = ImMax( r, ImMax( g, b ) );
  const float minChannel = ImMin( r, ImMin( g, b ) );
  const float delta      = maxChannel - minChannel;

  l = ( maxChannel + minChannel ) * 0.5f;
  if ( delta <= 0.000001f ) {
    h = 0.0f;
    s = 0.0f;
    return;
  }

  s = delta / ImMax( 0.000001f, 1.0f - fabsf( 2.0f * l - 1.0f ) );
  if ( maxChannel == r )
    h = fmodf( ( g - b ) / delta, 6.0f );
  else if ( maxChannel == g )
    h = ( b - r ) / delta + 2.0f;
  else
    h = ( r - g ) / delta + 4.0f;

  h /= 6.0f;
  if ( h < 0.0f ) h += 1.0f;
}

inline void ColorConvertHSLtoRGB( float h, float s, float l, float& r, float& g, float& b ) {
  h = h - floorf( h );
  s = ImClamp( s, 0.0f, 1.0f );
  l = ImClamp( l, 0.0f, 1.0f );

  const float c  = ( 1.0f - fabsf( 2.0f * l - 1.0f ) ) * s;
  const float h6 = h * 6.0f;
  const float x  = c * ( 1.0f - fabsf( fmodf( h6, 2.0f ) - 1.0f ) );
  const float m  = l - c * 0.5f;

  float rr = 0.0f, gg = 0.0f, bb = 0.0f;
  if ( h6 < 1.0f ) {
    rr = c; gg = x;
  } else if ( h6 < 2.0f ) {
    rr = x; gg = c;
  } else if ( h6 < 3.0f ) {
    gg = c; bb = x;
  } else if ( h6 < 4.0f ) {
    gg = x; bb = c;
  } else if ( h6 < 5.0f ) {
    rr = x; bb = c;
  } else {
    rr = c; bb = x;
  }

  r = rr + m;
  g = gg + m;
  b = bb + m;
}

inline int ExtractColorNumbers( const char* text, float* values, int capacity ) {
  int count = 0;
  const char* cursor = text;
  while ( cursor && *cursor && count < capacity ) {
    while ( *cursor && !( ( *cursor >= '0' && *cursor <= '9' ) || *cursor == '-' || *cursor == '+' || *cursor == '.' ) )
      ++cursor;
    if ( !*cursor ) break;

    char* end = nullptr;
    const float parsed = strtof( cursor, &end );
    if ( end == cursor ) {
      ++cursor;
      continue;
    }

    values[count++] = parsed;
    cursor = end;
  }
  return count;
}

inline void FormatColorValue( char* buffer, size_t bufferSize, const ImVec4& col, bool useAlpha, ColorValueFormat format ) {
  if ( format == ColorValueFormat::Hex ) {
    if ( useAlpha )
      sprintf_s( buffer, bufferSize, "#%02X%02X%02X%02X", ColorByte( col.x ), ColorByte( col.y ), ColorByte( col.z ), ColorByte( col.w ) );
    else
      sprintf_s( buffer, bufferSize, "#%02X%02X%02X", ColorByte( col.x ), ColorByte( col.y ), ColorByte( col.z ) );
    return;
  }

  if ( format == ColorValueFormat::Rgb ) {
    sprintf_s( buffer, bufferSize, "%d, %d, %d", ColorByte( col.x ), ColorByte( col.y ), ColorByte( col.z ) );
    return;
  }

  float h = 0.0f, s = 0.0f, third = 0.0f;
  if ( format == ColorValueFormat::Hsv )
    ImGui::ColorConvertRGBtoHSV( col.x, col.y, col.z, h, s, third );
  else
    ColorConvertRGBtoHSL( col.x, col.y, col.z, h, s, third );

  sprintf_s( buffer, bufferSize, "%.0f, %.0f%%, %.0f%%", h * 360.0f, s * 100.0f, third * 100.0f );
}

inline bool ParseColorValue( const char* text, ImVec4& col, bool useAlpha, ColorValueFormat format ) {
  if ( format == ColorValueFormat::Hex ) {
    char digits[9] = {};
    int digitCount = 0;
    for ( const char* cursor = text; cursor && *cursor && digitCount < 8; ++cursor ) {
      const bool isHex = ( *cursor >= '0' && *cursor <= '9' ) || ( *cursor >= 'a' && *cursor <= 'f' ) || ( *cursor >= 'A' && *cursor <= 'F' );
      if ( isHex ) digits[digitCount++] = *cursor;
    }

    const int requiredDigits = useAlpha ? 8 : 6;
    if ( digitCount != requiredDigits ) return false;

    const unsigned long packed = strtoul( digits, nullptr, 16 );
    if ( useAlpha ) {
      col.x = static_cast<float>( ( packed >> 24 ) & 0xFF ) / 255.0f;
      col.y = static_cast<float>( ( packed >> 16 ) & 0xFF ) / 255.0f;
      col.z = static_cast<float>( ( packed >> 8 ) & 0xFF ) / 255.0f;
      col.w = static_cast<float>( packed & 0xFF ) / 255.0f;
    } else {
      col.x = static_cast<float>( ( packed >> 16 ) & 0xFF ) / 255.0f;
      col.y = static_cast<float>( ( packed >> 8 ) & 0xFF ) / 255.0f;
      col.z = static_cast<float>( packed & 0xFF ) / 255.0f;
    }
    return true;
  }

  float values[3] = {};
  if ( ExtractColorNumbers( text, values, 3 ) != 3 ) return false;

  if ( format == ColorValueFormat::Rgb ) {
    col.x = ImClamp( values[0], 0.0f, 255.0f ) / 255.0f;
    col.y = ImClamp( values[1], 0.0f, 255.0f ) / 255.0f;
    col.z = ImClamp( values[2], 0.0f, 255.0f ) / 255.0f;
    return true;
  }

  const float h = fmodf( values[0], 360.0f ) / 360.0f;
  const float s = ImClamp( values[1], 0.0f, 100.0f ) / 100.0f;
  const float third = ImClamp( values[2], 0.0f, 100.0f ) / 100.0f;
  if ( format == ColorValueFormat::Hsv )
    ImGui::ColorConvertHSVtoRGB( h < 0.0f ? h + 1.0f : h, s, third, col.x, col.y, col.z );
  else
    ColorConvertHSLtoRGB( h < 0.0f ? h + 1.0f : h, s, third, col.x, col.y, col.z );
  return true;
}

inline void FormatColorComponent( char* buffer, size_t bufferSize, const ImVec4& col, ColorValueFormat format, int component ) {
  if ( format == ColorValueFormat::Rgb ) {
    const char labels[] = { 'R', 'G', 'B' };
    const float channels[] = { col.x, col.y, col.z };
    sprintf_s( buffer, bufferSize, "%c %d", labels[component], ColorByte( channels[component] ) );
    return;
  }

  float h = 0.0f, s = 0.0f, third = 0.0f;
  if ( format == ColorValueFormat::Hsv )
    ImGui::ColorConvertRGBtoHSV( col.x, col.y, col.z, h, s, third );
  else
    ColorConvertRGBtoHSL( col.x, col.y, col.z, h, s, third );

  const char thirdLabel = ( format == ColorValueFormat::Hsv ) ? 'V' : 'L';
  if ( component == 0 )
    sprintf_s( buffer, bufferSize, "H %.0f", h * 360.0f );
  else if ( component == 1 )
    sprintf_s( buffer, bufferSize, "S %.0f%%", s * 100.0f );
  else
    sprintf_s( buffer, bufferSize, "%c %.0f%%", thirdLabel, third * 100.0f );
}

inline bool ApplyColorComponent( const char* text, ImVec4& col, ColorValueFormat format, int component ) {
  float parsed[1] = {};
  if ( ExtractColorNumbers( text, parsed, 1 ) != 1 ) return false;

  if ( format == ColorValueFormat::Rgb ) {
    const float channel = ImClamp( parsed[0], 0.0f, 255.0f ) / 255.0f;
    if ( component == 0 ) col.x = channel;
    if ( component == 1 ) col.y = channel;
    if ( component == 2 ) col.z = channel;
    return true;
  }

  float h = 0.0f, s = 0.0f, third = 0.0f;
  if ( format == ColorValueFormat::Hsv )
    ImGui::ColorConvertRGBtoHSV( col.x, col.y, col.z, h, s, third );
  else
    ColorConvertRGBtoHSL( col.x, col.y, col.z, h, s, third );

  if ( component == 0 ) {
    h = fmodf( parsed[0], 360.0f ) / 360.0f;
    if ( h < 0.0f ) h += 1.0f;
  } else if ( component == 1 ) {
    s = ImClamp( parsed[0], 0.0f, 100.0f ) / 100.0f;
  } else {
    third = ImClamp( parsed[0], 0.0f, 100.0f ) / 100.0f;
  }

  if ( format == ColorValueFormat::Hsv )
    ImGui::ColorConvertHSVtoRGB( h, s, third, col.x, col.y, col.z );
  else
    ColorConvertHSLtoRGB( h, s, third, col.x, col.y, col.z );
  return true;
}

inline bool RenderAdvancedColorValueEditor( ImVec4& col, bool useAlpha, float width ) {
  struct EditorState {
    std::array<char, 64> value{};
    std::array<std::array<char, 24>, 3> components{};
    std::array<char, 16> alpha{};
    ColorValueFormat lastFormat = ColorValueFormat::Hex;
    bool initialized = false;
  };

  ImGui::PushID( "##advanced_color_value_editor" );
  const ImGuiID editorId = ImGui::GetID( "##state" );
  static std::unordered_map<ImGuiID, EditorState> states;
  EditorState& state = states[editorId];

  const float spacing       = 4.0f;
  const float iconSpacing   = 1.0f;
  const float rowHeight     = 20.0f;
  const float selectorW     = 64.0f;
  const float alphaW        = useAlpha ? 42.0f : 0.0f;
  const float actionW       = 20.0f;
  const float previewW      = 24.0f;
  const float rightClusterW = previewW + spacing + actionW + iconSpacing + actionW;
  const float rowStartX     = ImGui::GetCursorPosX();
  bool changed              = false;

  const ImVec2 currentFramePadding = ImGui::GetStyle().FramePadding;
  ImGui::PushStyleVar( ImGuiStyleVar_FramePadding, ImVec2( currentFramePadding.x, ImMax( 0.0f, ( rowHeight - ImGui::GetFontSize() ) * 0.5f ) ) );

  int formatIndex = static_cast<int>( colorValueFormat );
  const std::vector<const char*> formatItems = { "HEX", "RGB", "HSV", "HSL" };
  if ( MenuWidgets::Combo( "##format", &formatIndex, formatItems, nullptr, false, ImVec2( selectorW, rowHeight ) ) )
    colorValueFormat = static_cast<ColorValueFormat>( formatIndex );

  // Alpha sits directly between the format selector and the wide preview.
  if ( useAlpha ) {
    const ImGuiID alphaInputId = ImGui::GetID( "##alpha_percent" );
    const bool alphaActive = ImGui::GetActiveID() == alphaInputId;
    if ( !state.initialized || !alphaActive )
      sprintf_s( state.alpha.data(), state.alpha.size(), "%.0f%%", ImClamp( col.w, 0.0f, 1.0f ) * 100.0f );

    ImGui::SameLine( 0.0f, spacing );
    ImGui::SetNextItemWidth( alphaW );
    const bool alphaSubmitted = ImGui::InputText( "##alpha_percent", state.alpha.data(), state.alpha.size(), ImGuiInputTextFlags_EnterReturnsTrue );
    if ( alphaSubmitted || ImGui::IsItemDeactivatedAfterEdit() ) {
      float alphaValues[1] = {};
      if ( ExtractColorNumbers( state.alpha.data(), alphaValues, 1 ) == 1 ) {
        col.w = ImClamp( alphaValues[0], 0.0f, 100.0f ) / 100.0f;
        sprintf_s( state.alpha.data(), state.alpha.size(), "%.0f%%", col.w * 100.0f );
        changed = true;
      }
    }
  }

  ImGui::SameLine();
  ImGui::SetCursorPosX( rowStartX + width - rightClusterW );
  ImGui::ColorButton( "##value_preview", col, ImGuiColorEditFlags_NoPicker | ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_AlphaPreviewHalf, ImVec2( previewW, rowHeight ) );

  if ( !icon_copy_tex && g_pd3dDevice )
    TextureLoader::LoadFromMemory( g_pd3dDevice, icons::copy, sizeof( icons::copy ), &icon_copy_tex, nullptr, nullptr, 32, 32 );
  if ( !icon_paste_tex && g_pd3dDevice )
    TextureLoader::LoadFromMemory( g_pd3dDevice, icons::paste, sizeof( icons::paste ), &icon_paste_tex, nullptr, nullptr, 32, 32 );

  ImGui::SameLine( 0.0f, spacing );
  bool copyClicked = false;
  DrawIconButton( "##copy_btn", icon_copy_tex, ImVec2( actionW, rowHeight ), copyClicked, false );
  if ( copyClicked ) {
    copiedColor = col;
    hasColorCopied = true;
  }

  ImGui::SameLine( 0.0f, iconSpacing );
  bool pasteClicked = false;
  DrawIconButton( "##paste_btn", icon_paste_tex, ImVec2( actionW, rowHeight ), pasteClicked, hasColorCopied );
  if ( pasteClicked && hasColorCopied ) {
    col = copiedColor;
    changed = true;
  }

  // Every format uses the same lower value row, including HEX.
  ImGui::SetCursorPosX( rowStartX );
  ImGui::Dummy( ImVec2( 0.0f, 2.0f ) );
  ImGui::SetCursorPosX( rowStartX );

  const float addButtonW = 20.0f;
  const float inputAreaW = width - spacing - addButtonW;

  if ( colorValueFormat == ColorValueFormat::Hex ) {
    const ImGuiID valueInputId = ImGui::GetID( "##value" );
    const bool valueActive = ImGui::GetActiveID() == valueInputId;
    if ( !state.initialized || !valueActive || state.lastFormat != colorValueFormat )
      FormatColorValue( state.value.data(), state.value.size(), col, useAlpha, colorValueFormat );

    ImGui::SetNextItemWidth( inputAreaW );
    const bool valueSubmitted = ImGui::InputText( "##value", state.value.data(), state.value.size(), ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CharsUppercase );
    if ( ( valueSubmitted || ImGui::IsItemDeactivatedAfterEdit() ) && ParseColorValue( state.value.data(), col, useAlpha, colorValueFormat ) ) {
      FormatColorValue( state.value.data(), state.value.size(), col, useAlpha, colorValueFormat );
      changed = true;
    }
  } else {

    const float componentW = ( inputAreaW - spacing * 2.0f ) / 3.0f;
    for ( int component = 0; component < 3; ++component ) {
      if ( component > 0 ) ImGui::SameLine( 0.0f, spacing );
      ImGui::PushID( component );

      const ImGuiID componentId = ImGui::GetID( "##component" );
      const bool componentActive = ImGui::GetActiveID() == componentId;
      if ( !state.initialized || !componentActive || state.lastFormat != colorValueFormat )
        FormatColorComponent( state.components[component].data(), state.components[component].size(), col, colorValueFormat, component );

      ImGui::SetNextItemWidth( componentW );
      const bool submitted = ImGui::InputText( "##component", state.components[component].data(), state.components[component].size(), ImGuiInputTextFlags_EnterReturnsTrue );
      if ( ( submitted || ImGui::IsItemDeactivatedAfterEdit() ) && ApplyColorComponent( state.components[component].data(), col, colorValueFormat, component ) ) {
        FormatColorComponent( state.components[component].data(), state.components[component].size(), col, colorValueFormat, component );
        changed = true;
      }

      ImGui::PopID();
    }
  }

  ImGui::SameLine( 0.0f, spacing );
  const bool canAddToPalette = globals::SavedPaletteCount < kVisiblePaletteCapacity;
 
  if ( !canAddToPalette ) ImGui::BeginDisabled();
  const bool addToPalette = ImGui::Button( "+##add_palette", ImVec2( addButtonW, rowHeight ) );
  if ( !canAddToPalette ) ImGui::EndDisabled();
  ImGui::PopStyleVar( 2 );
  ImGui::PopStyleColor( 5 );

  if ( addToPalette && canAddToPalette ) {
    const int slot = ImClamp( globals::SavedPaletteCount, 0, kVisiblePaletteCapacity - 1 );
    globals::SavedPalette[slot] = col;
    globals::SavedPaletteCount = slot + 1;
    ConfigManager::SaveConfig( ConfigManager::GetConfigPath(), ConfigManager::GetSelectedConfig() );
  }

  state.lastFormat = colorValueFormat;
  state.initialized = true;
  ImGui::PopStyleVar();
  ImGui::PopID();
  return changed;
}

inline bool RenderColorPalette( ImVec4& col ) {
  bool changed = false;

  const int paletteCount = ImClamp( globals::SavedPaletteCount, 0, kVisiblePaletteCapacity );
  if ( paletteCount <= 0 ) return false;

  ImGui::Dummy( ImVec2( 0.0f, 2.0f ) );

  const float spacing      = 3.0f;
  const float slot_w       = 24.0f;
  const float slot_h       = 20.0f;

  ImGui::PushStyleVar( ImGuiStyleVar_ItemSpacing, ImVec2( spacing, spacing ) );
  ImGui::PushStyleVar( ImGuiStyleVar_FrameRounding, 3.0f );
  ImGui::PushStyleVar( ImGuiStyleVar_FrameBorderSize, 1.0f );
  ImGui::PushStyleColor( ImGuiCol_Border, ImVec4( 0.15f, 0.15f, 0.15f, 1.0f ) );

  for ( int i = 0; i < paletteCount; i++ ) {
    ImGui::PushID( i );
    if ( i > 0 ) ImGui::SameLine( 0.0f, spacing );

    bool is_selected = ( globals::SavedPalette[i].x == col.x &&
                         globals::SavedPalette[i].y == col.y &&
                         globals::SavedPalette[i].z == col.z &&
                         globals::SavedPalette[i].w == col.w );

    if ( is_selected ) {
      ImGui::PushStyleColor( ImGuiCol_Border, ImVec4( 0.7f, 0.7f, 0.7f, 1.0f ) );
      ImGui::PushStyleVar( ImGuiStyleVar_FrameBorderSize, 1.5f );
    }

    if ( ImGui::ColorButton( "##palette_slot", globals::SavedPalette[i], ImGuiColorEditFlags_NoPicker | ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_AlphaPreviewHalf, ImVec2( slot_w, slot_h ) ) ) {
      col     = globals::SavedPalette[i];
      changed = true;
    }

    if ( is_selected ) {
      ImGui::PopStyleVar();
      ImGui::PopStyleColor();
    }

    if ( ImGui::IsItemHovered() ) {
      if ( ImGui::IsMouseClicked( 1 ) ) {
        globals::SavedPalette[i] = col;
        ConfigManager::SaveConfig( ConfigManager::GetConfigPath(), ConfigManager::GetSelectedConfig() );
      }
    }
    ImGui::PopID();
  }

  ImGui::PopStyleColor();
  ImGui::PopStyleVar( 3 );
  return changed;
}

// Function declarations (kept for backward compatibility)
bool CustomCheckbox( const char* label, bool* v );
bool CustomCombo( const char* label, int* current_item, const std::vector<std::string>& items );

// Helper function for easy replacement of ImGui::Checkbox calls
inline bool AnimatedCheckbox( const char* label, bool* v ) {
  return CustomCheckbox( label, v );
}

// Custom Radio Button with Checkbox style
inline bool CustomRadioButton( const char* label, int* v, int v_button ) {
    ImGui::PushID( v );
    ImGui::PushID( v_button );

    const float rowH = 24.0f;
    const float box  = 12.0f;
    const float mr   = 12.0f;

    ImFont* font      = menuFont_px11 ? menuFont_px11 : ImGui::GetFont();
    const float fontSize = font ? font->FontSize : ImGui::GetFontSize();

    ImVec2 textSize = font->CalcTextSizeA( fontSize, FLT_MAX, 0.0f, label );
    float   minWidth = box + mr + textSize.x + ImGui::GetStyle().FramePadding.x * 2.0f;
    float   availW   = ImGui::GetContentRegionAvail().x;
    float   w        = ImMax( minWidth, availW );
    float   h        = rowH;

    bool pressed = ImGui::InvisibleButton( "##radio", ImVec2( w, h ) );
    if ( pressed )
      *v = v_button;

    bool hovered = ImGui::IsItemHovered();
    bool active  = ( *v == v_button );

    ImDrawList* draw = ImGui::GetWindowDrawList();
    ImVec2 pMin      = ImGui::GetItemRectMin();
    ImVec2 pMax      = ImGui::GetItemRectMax();
    float  centerY   = ( pMin.y + pMax.y ) * 0.5f;
    ImVec2 checkMin( pMin.x, centerY - box * 0.5f );
    ImVec2 checkMax( checkMin.x + box, checkMin.y + box );
    ImVec2 center( ( checkMin.x + checkMax.x ) * 0.5f, ( checkMin.y + checkMax.y ) * 0.5f );
    const float radius = box * 0.5f;

  auto& motion = WindowMotion::System();
    const auto& tokens = WindowMotion::Tokens();
    const std::string motionId = std::to_string( static_cast<ImU32>( ImGui::GetItemID() ) );
    const float hoverT = motion.value( ui::motion::MotionKey( "radio", motionId, "hover" ), hovered ? 1.0f : 0.0f, tokens.hoverFast, hovered ? 1.0f : 0.0f );
    const float fillT  = motion.value( ui::motion::MotionKey( "radio", motionId, "fill" ), active ? 1.0f : 0.0f, tokens.pressFast, active ? 1.0f : 0.0f );

    if ( fillT > 0.001f ) {
      const float  maxInset = box * 0.5f;
      const float  inset    = maxInset * ( 1.0f - fillT );
      const ImVec2 fMin( checkMin.x + inset, checkMin.y + inset );
      const ImVec2 fMax( checkMax.x - inset, checkMax.y - inset );
      ImVec4       fillCol = colors::accent;
      fillCol.w *= fillT;
      draw->AddRectFilled( fMin, fMax, ImGui::GetColorU32( fillCol ), 0.0f );
    }

    const ImVec4 borderOff = hovered ? ImVec4( 0.333f, 0.333f, 0.333f, 1.0f ) : ImVec4( 0.2f, 0.2f, 0.2f, 1.0f );
    const ImVec4 borderOn  = colors::accent;
    const ImVec4 borderFinal(
        borderOff.x + ( borderOn.x - borderOff.x ) * fillT,
        borderOff.y + ( borderOn.y - borderOff.y ) * fillT,
        borderOff.z + ( borderOn.z - borderOff.z ) * fillT,
        borderOff.w + ( borderOn.w - borderOff.w ) * fillT );
    draw->AddRect( checkMin, checkMax, ImGui::GetColorU32( borderFinal ), 0.0f );

    float textHeight = font ? ( font->Ascent - font->Descent ) : fontSize;
    float textY      = floorf( centerY - textHeight * 0.5f + 0.5f );
    ImVec2 textPos( pMin.x + box + mr, textY );

    ImU32 textCol;
    if ( active ) {
      textCol = ImGui::GetColorU32( colors::text );
    } else {
      ImVec4 colBase  = colors::textDim;
      ImVec4 colHover = ImVec4( 0.8f, 0.8f, 0.8f, 1.0f );
      ImVec4 finalTextCol(
          colBase.x + ( colHover.x - colBase.x ) * hoverT,
          colBase.y + ( colHover.y - colBase.y ) * hoverT,
          colBase.z + ( colHover.z - colBase.z ) * hoverT,
          colBase.w + ( colHover.w - colBase.w ) * hoverT );
      textCol = ImGui::GetColorU32( finalTextCol );
    }

    draw->AddText( font, fontSize, textPos, textCol, label );

    ImGui::PopID();
    ImGui::PopID();
    return pressed;
}

inline void CustomColorPicker( const char* label, ImVec4& col, bool useAlpha, float Width ) {
  ImGui::PushItemWidth( Width );  // Begrenzt die Breite der Widgets

  bool   colorChanged      = false;

  colorChanged = useAlpha
                 ? ImGui::ColorPicker4( "##picker", (float*)&col,
                                        ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoSidePreview | ImGuiColorEditFlags_NoOptions | ImGuiColorEditFlags_PickerHueBar | ImGuiColorEditFlags_InputRGB )
                 : ImGui::ColorPicker3( "##picker", (float*)&col,
                                        ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoSidePreview | ImGuiColorEditFlags_NoOptions | ImGuiColorEditFlags_PickerHueBar | ImGuiColorEditFlags_InputRGB );

  ImGui::Dummy( ImVec2( 0.0f, 1.0f ) );
  colorChanged |= RenderAdvancedColorValueEditor( col, useAlpha, Width );

  if ( RenderColorPalette( col ) ) {
    colorChanged = true;
  }

  // Animation removed to fix snap for color palette
  
  ImGui::PopItemWidth();
}

inline void CustomColorPicker4( const char* label, ImVec4& col, float Width ) {
  CustomColorPicker( label, col, true, Width );
}

inline void CustomColorPicker3( const char* label, ImVec4& col, float Width ) {
  CustomColorPicker( label, col, false, Width );
}

inline ImVec2 GetAnchoredColorPopupPos( const ImVec2& anchorMin, const ImVec2& anchorMax ) {
  const float  popupGap        = 4.0f;
  const float  popupWidth      = GetAdvancedColorPickerWidth() + 12.0f;
  const ImVec2 vpPos           = ImGui::GetMainViewport()->Pos;
  const ImVec2 vpSize          = ImGui::GetMainViewport()->Size;
  const float  vpMaxX          = vpPos.x + vpSize.x;

  ImVec2 popupPos( anchorMin.x, anchorMax.y + popupGap );
  popupPos.x = ImClamp( popupPos.x, vpPos.x, vpMaxX - popupWidth );

  return popupPos;
}

inline bool IsPopupAnchorVisibleInCurrentWindow( const ImVec2& anchorMin, const ImVec2& anchorMax ) {
  ImGuiWindow* ownerWindow = ImGui::GetCurrentWindow();
  const ImVec2 clipMin     = ownerWindow ? ownerWindow->ClipRect.Min : anchorMin;
  const ImVec2 clipMax     = ownerWindow ? ownerWindow->ClipRect.Max : anchorMax;
  return anchorMin.x < clipMax.x && anchorMax.x > clipMin.x && anchorMin.y < clipMax.y && anchorMax.y > clipMin.y;
}

inline bool CanPopupStartBelowCurrentWindow( const ImVec2& anchorMax ) {
  const float  popupGap   = 4.0f;
  ImGuiWindow* ownerWindow = ImGui::GetCurrentWindow();
  const float  windowMaxY  = ownerWindow ? ownerWindow->ClipRect.Max.y : ( anchorMax.y + popupGap + 1.0f );
  const float  popupTopY  = anchorMax.y + popupGap;
  return popupTopY < windowMaxY;
}

inline void CustomColorEditSized( const char* label, ImVec4& col, bool useAlpha, float size ) {
  std::string popupLabel       = std::string( label ) + "##popup";
  std::string contextMenuLabel = std::string( label ) + "##contextmenu";
  const ImGuiID stateId        = ImGui::GetID( ( std::string( label ) + "##color_popup_state" ).c_str() );

  ImVec2 buttonSize;
  if ( size > 0.0f ) {
    buttonSize = ImVec2( size, size );
  } else {
    // Slightly smaller default swatch to match the menu's compact density.
    buttonSize = ImGui::CalcItemSize( ImVec2( 24.f, 8.f ), 0, 0 );
    buttonSize.x += ImGui::GetStyle().FramePadding.x * 2;
    buttonSize.y += ImGui::GetStyle().FramePadding.y * 2;
  }

  static std::unordered_map<ImGuiID, bool>   popupJustOpened;
  static std::unordered_map<ImGuiID, ImVec2> popupAnchorMin;
  static std::unordered_map<ImGuiID, ImVec2> popupAnchorMax;
  bool colorPressed = ImGui::ColorButton( label, col, ImGuiColorEditFlags_NoPicker | ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_AlphaPreviewHalf, buttonSize );
  popupAnchorMin[stateId] = ImGui::GetItemRectMin();
  popupAnchorMax[stateId] = ImGui::GetItemRectMax();

  if ( colorPressed ) {
    ImGui::OpenPopup( popupLabel.c_str() );
    WindowMotion::System().set( ui::motion::MotionKey( "color-popup", std::to_string( static_cast<ImU32>( stateId ) ), "alpha" ), 0.0f );
    WindowMotion::System().set( ui::motion::MotionKey( "color-popup", std::to_string( static_cast<ImU32>( stateId ) ), "slide" ), 0.0f );
    popupJustOpened[stateId] = true;
  }

  // Hover glow for the swatch (1px subtle outline).
  {
    const bool hovered = ImGui::IsItemHovered();
    const ImGuiID itemId = ImGui::GetItemID();
    const float t = WindowMotion::System().value( ui::motion::MotionKey( "color-swatch", std::to_string( static_cast<ImU32>( itemId ) ), "hover" ), hovered ? 1.0f : 0.0f, WindowMotion::Tokens().hoverFast, hovered ? 1.0f : 0.0f );

    if (t > 0.001f) {
      ImDrawList* draw = ImGui::GetWindowDrawList();
      ImVec2 rMin = ImGui::GetItemRectMin();
      ImVec2 rMax = ImGui::GetItemRectMax();
      float lum = col.x * 0.299f + col.y * 0.587f + col.z * 0.114f;
      float boost = (lum < 0.15f) ? 0.3f : 0.2f;
      float gr = ImMin(col.x + boost, 1.0f);
      float gg = ImMin(col.y + boost, 1.0f);
      float gb = ImMin(col.z + boost, 1.0f);
      // Subtle 2-layer soft glow
      const int layers = 2;
      for (int i = layers; i >= 1; --i) {
        float expand = (float)i * 1.0f;
        float alpha = (0.10f / (float)i) * t;
        draw->AddRectFilled(
            ImVec2(rMin.x - expand, rMin.y - expand),
            ImVec2(rMax.x + expand, rMax.y + expand),
            ImGui::GetColorU32(ImVec4(gr, gg, gb, alpha)),
            1.5f + expand * 0.5f);
      }
    }
  }

  if ( ImGui::BeginPopupContextItem( contextMenuLabel.c_str() ) ) {
    ImGui::PushStyleVar( ImGuiStyleVar_ItemSpacing, ImVec2( 0, 2 ) );
    ImGui::PushStyleVar( ImGuiStyleVar_FramePadding, ImVec2( 8.0f, 4.0f ) );
    ImGui::PushStyleVar( ImGuiStyleVar_ButtonTextAlign, ImVec2( 0.5f, 0.7f ) );
    ImGui::PushStyleColor( ImGuiCol_Button, colors::comboBg );
    ImGui::PushStyleColor( ImGuiCol_ButtonHovered, ImVec4( 0.067f, 0.067f, 0.067f, 1.0f ) );
    ImGui::PushStyleColor( ImGuiCol_ButtonActive, ImVec4( 0.1f, 0.1f, 0.1f, 1.0f ) );

    if ( ImGui::Button( "Copy", ImVec2( 60.0f, 0 ) ) ) {
      copiedColor    = col;
      hasColorCopied = true;
      ImGui::CloseCurrentPopup();
    }
    if ( ImGui::Button( "Paste", ImVec2( 60.0f, 0 ) ) ) {
      if ( hasColorCopied ) {
        col = copiedColor;
      }
      ImGui::CloseCurrentPopup();
    }

    ImGui::PopStyleColor( 3 );
    ImGui::PopStyleVar( 3 );
    ImGui::EndPopup();
  }

  const bool isPopupOpen     = ImGui::IsPopupOpen( popupLabel.c_str() );
  const bool anchorVisible   = IsPopupAnchorVisibleInCurrentWindow( popupAnchorMin[stateId], popupAnchorMax[stateId] );
  const bool canStartBelow  = CanPopupStartBelowCurrentWindow( popupAnchorMax[stateId] );
  const bool shouldAutoClose = isPopupOpen && ( !anchorVisible || !canStartBelow );

  auto&   motion     = WindowMotion::System();
  const auto& tokens = WindowMotion::Tokens();
  float popupAlpha   = motion.value( ui::motion::MotionKey( "color-popup", std::to_string( static_cast<ImU32>( stateId ) ), "alpha" ), isPopupOpen ? 1.0f : 0.0f, isPopupOpen ? tokens.fadeFast : tokens.fadeFast, 0.0f );
  float popupSlide   = motion.value( ui::motion::MotionKey( "color-popup", std::to_string( static_cast<ImU32>( stateId ) ), "slide" ), isPopupOpen ? 1.0f : 0.0f, isPopupOpen ? tokens.springSnappy : tokens.fadeFast, 0.0f );

  if ( isPopupOpen ) {
    if ( popupAnchorMin.find( stateId ) != popupAnchorMin.end() && popupAnchorMax.find( stateId ) != popupAnchorMax.end() ) {
      const ImVec2 basePopupPos = GetAnchoredColorPopupPos( popupAnchorMin[stateId], popupAnchorMax[stateId] );
      const ImVec2 animatedPopupPos( basePopupPos.x, basePopupPos.y + ( 1.0f - popupSlide ) * 6.0f );
      ImGui::SetNextWindowPos( animatedPopupPos );
    }
  }

  // v2 popup styling
  ImGui::PushStyleColor( ImGuiCol_PopupBg, colors::comboBg );
  ImGui::PushStyleColor( ImGuiCol_Border, colors::border );
  ImGui::PushStyleVar( ImGuiStyleVar_PopupBorderSize, 1.0f );

  if ( ImGui::BeginPopup( popupLabel.c_str() ) ) {
    if ( shouldAutoClose ) {
      WindowMotion::System().set( ui::motion::MotionKey( "color-popup", std::to_string( static_cast<ImU32>( stateId ) ), "alpha" ), 0.0f );
      WindowMotion::System().set( ui::motion::MotionKey( "color-popup", std::to_string( static_cast<ImU32>( stateId ) ), "slide" ), 0.0f );
      popupJustOpened[stateId] = false;
      ImGui::CloseCurrentPopup();
      ImGui::EndPopup();
      ImGui::PopStyleVar();
      ImGui::PopStyleColor( 2 );
      return;
    }

    ImGui::PushStyleVar( ImGuiStyleVar_Alpha, popupAlpha );
    ImGui::PushStyleVar( ImGuiStyleVar_WindowPadding, ImVec2( 6.0f, 6.0f ) );
    ImGui::PushStyleVar( ImGuiStyleVar_ItemSpacing, ImVec2( 6.0f, 2.0f ) );

    const float picker_w = GetAdvancedColorPickerWidth();
    ImGui::SetNextItemWidth( picker_w );
    bool colorChanged = useAlpha
                            ? ImGui::ColorPicker4( "##picker", (float*)&col, ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoSidePreview | ImGuiColorEditFlags_NoOptions | ImGuiColorEditFlags_PickerHueBar | ImGuiColorEditFlags_InputRGB )
                            : ImGui::ColorPicker3( "##picker", (float*)&col, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoSidePreview | ImGuiColorEditFlags_NoOptions | ImGuiColorEditFlags_PickerHueBar | ImGuiColorEditFlags_InputRGB );

    ImGui::Dummy( ImVec2( 0.0f, 1.0f ) );

    const float full_w = picker_w;
    ImGui::PushStyleVar( ImGuiStyleVar_FramePadding, ImVec2( 5.0f, 3.0f ) );
    colorChanged |= RenderAdvancedColorValueEditor( col, useAlpha, full_w );
    ImGui::PopStyleVar();

    if ( RenderColorPalette( col ) ) {
      colorChanged = true;
    }

    // Animation removed to fix snap for color palette

    // OK button removed as requested


    // Pop compact style + alpha.
    ImGui::PopStyleVar( 3 );
    ImGui::EndPopup();
  } else {
    // Reset popup state when popup is closed
    if ( popupJustOpened.find( stateId ) != popupJustOpened.end() && popupJustOpened[stateId] ) {
      popupJustOpened[stateId] = false;
    }
  }
  ImGui::PopStyleVar();
  ImGui::PopStyleColor( 2 );
}

inline void CustomColorEdit( const char* label, ImVec4& col, bool useAlpha ) {
  CustomColorEditSized( label, col, useAlpha, -1.0f );
}

inline void CustomColorEdit4( const char* label, ImVec4& col ) {
  CustomColorEditSized( label, col, true, -1.0f );
}

inline void CustomColorEdit3( const char* label, ImVec4& col ) {
  CustomColorEditSized( label, col, false, -1.0f );
}

inline void CustomColorEdit4Sized( const char* label, ImVec4& col, float size ) {
  CustomColorEditSized( label, col, true, size );
}

inline void CustomColorEdit3Sized( const char* label, ImVec4& col, float size ) {
  CustomColorEditSized( label, col, false, size );
}

// Dual color picker for gradients - shows 2 color boxes side by side
inline void CustomDualColorEditSized( const char* label, ImVec4& col1, ImVec4& col2, bool useAlpha, float size ) {
  std::string popupLabel = std::string( label ) + "##dualpopup";
  const ImGuiID stateId  = ImGui::GetID( ( std::string( label ) + "##dual_color_popup_state" ).c_str() );

  float boxSize = ( size > 0.0f ) ? size : 12.0f;
  float gap     = 2.0f;

  // Store which color is being edited (0 = none/col1, 1 = col2)
  static std::unordered_map<ImGuiID, int> activeColorIndex;
  if ( activeColorIndex.find( stateId ) == activeColorIndex.end() ) {
    activeColorIndex[stateId] = 0;
  }
  int& editIndex = activeColorIndex[stateId];

  static std::unordered_map<ImGuiID, bool>   popupJustOpened;
  static std::unordered_map<ImGuiID, ImVec2> popupAnchorMin;
  static std::unordered_map<ImGuiID, ImVec2> popupAnchorMax;
  ImGui::PushID( label );

  // Draw both color boxes side by side using manual positioning
  ImVec2 startPos = ImGui::GetCursorPos();
  ImVec2 startScreenPos = ImGui::GetCursorScreenPos();

  // First color box
  bool firstPressed = ImGui::ColorButton( "##c1", col1, ImGuiColorEditFlags_NoPicker | ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_AlphaPreviewHalf, ImVec2( boxSize, boxSize ) );
  if ( editIndex == 0 || !ImGui::IsPopupOpen( popupLabel.c_str() ) ) {
    popupAnchorMin[stateId] = ImGui::GetItemRectMin();
    popupAnchorMax[stateId] = ImGui::GetItemRectMax();
  }
  if ( firstPressed ) {
    editIndex = 0;
    popupAnchorMin[stateId] = ImGui::GetItemRectMin();
    popupAnchorMax[stateId] = ImGui::GetItemRectMax();
    ImGui::OpenPopup( popupLabel.c_str() );
    WindowMotion::System().set( ui::motion::MotionKey( "color-popup", std::to_string( static_cast<ImU32>( stateId ) ), "alpha" ), 0.0f );
    WindowMotion::System().set( ui::motion::MotionKey( "color-popup", std::to_string( static_cast<ImU32>( stateId ) ), "slide" ), 0.0f );
    popupJustOpened[stateId] = true;
  }

  // Hover glow for color box 1
  {
    const bool hovered = ImGui::IsItemHovered();
    const ImGuiID itemId = ImGui::GetItemID();
    const float t = WindowMotion::System().value( ui::motion::MotionKey( "color-swatch", std::to_string( static_cast<ImU32>( itemId ) ), "hover" ), hovered ? 1.0f : 0.0f, WindowMotion::Tokens().hoverFast, hovered ? 1.0f : 0.0f );

    if (t > 0.001f) {
      ImDrawList* draw = ImGui::GetWindowDrawList();
      ImVec2 rMin = ImGui::GetItemRectMin();
      ImVec2 rMax = ImGui::GetItemRectMax();
      float lum = col1.x * 0.299f + col1.y * 0.587f + col1.z * 0.114f;
      float boost = (lum < 0.15f) ? 0.3f : 0.2f;
      float gr = ImMin(col1.x + boost, 1.0f);
      float gg = ImMin(col1.y + boost, 1.0f);
      float gb = ImMin(col1.z + boost, 1.0f);
      const int layers = 2;
      for (int i = layers; i >= 1; --i) {
        float expand = (float)i * 1.0f;
        float alpha = (0.10f / (float)i) * t;
        draw->AddRectFilled(
            ImVec2(rMin.x - expand, rMin.y - expand),
            ImVec2(rMax.x + expand, rMax.y + expand),
            ImGui::GetColorU32(ImVec4(gr, gg, gb, alpha)),
            1.5f + expand * 0.5f);
      }
    }
  }

  // Context menu for first color
  if ( ImGui::BeginPopupContextItem( "##ctx1" ) ) {
    ImGui::PushStyleVar( ImGuiStyleVar_ItemSpacing, ImVec2( 0, 2 ) );
    ImGui::PushStyleVar( ImGuiStyleVar_FramePadding, ImVec2( 8.0f, 4.0f ) );
    ImGui::PushStyleVar( ImGuiStyleVar_ButtonTextAlign, ImVec2( 0.5f, 0.7f ) );
    ImGui::PushStyleColor( ImGuiCol_Button, colors::comboBg );
    ImGui::PushStyleColor( ImGuiCol_ButtonHovered, ImVec4( 0.067f, 0.067f, 0.067f, 1.0f ) );
    ImGui::PushStyleColor( ImGuiCol_ButtonActive, ImVec4( 0.1f, 0.1f, 0.1f, 1.0f ) );
    if ( ImGui::Button( "Copy", ImVec2( 60.0f, 0 ) ) ) {
      copiedColor    = col1;
      hasColorCopied = true;
      ImGui::CloseCurrentPopup();
    }
    if ( ImGui::Button( "Paste", ImVec2( 60.0f, 0 ) ) ) {
      if ( hasColorCopied ) col1 = copiedColor;
      ImGui::CloseCurrentPopup();
    }
    ImGui::PopStyleColor( 3 );
    ImGui::PopStyleVar( 3 );
    ImGui::EndPopup();
  }

  // Position second box manually (same Y, offset X)
  ImGui::SetCursorPos( ImVec2( startPos.x + boxSize + gap, startPos.y ) );

  // Second color box
  bool secondPressed = ImGui::ColorButton( "##c2", col2, ImGuiColorEditFlags_NoPicker | ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_AlphaPreviewHalf, ImVec2( boxSize, boxSize ) );
  if ( editIndex == 1 ) {
    popupAnchorMin[stateId] = ImGui::GetItemRectMin();
    popupAnchorMax[stateId] = ImGui::GetItemRectMax();
  }
  if ( secondPressed ) {
    editIndex = 1;
    popupAnchorMin[stateId] = ImGui::GetItemRectMin();
    popupAnchorMax[stateId] = ImGui::GetItemRectMax();
    ImGui::OpenPopup( popupLabel.c_str() );
    WindowMotion::System().set( ui::motion::MotionKey( "color-popup", std::to_string( static_cast<ImU32>( stateId ) ), "alpha" ), 0.0f );
    WindowMotion::System().set( ui::motion::MotionKey( "color-popup", std::to_string( static_cast<ImU32>( stateId ) ), "slide" ), 0.0f );
    popupJustOpened[stateId] = true;
  }

  // Hover glow for color box 2
  {
    const bool hovered = ImGui::IsItemHovered();
    const ImGuiID itemId = ImGui::GetItemID();
    const float t = WindowMotion::System().value( ui::motion::MotionKey( "color-swatch", std::to_string( static_cast<ImU32>( itemId ) ), "hover" ), hovered ? 1.0f : 0.0f, WindowMotion::Tokens().hoverFast, hovered ? 1.0f : 0.0f );

    if (t > 0.001f) {
      ImDrawList* draw = ImGui::GetWindowDrawList();
      ImVec2 rMin = ImGui::GetItemRectMin();
      ImVec2 rMax = ImGui::GetItemRectMax();
      float lum = col2.x * 0.299f + col2.y * 0.587f + col2.z * 0.114f;
      float boost = (lum < 0.15f) ? 0.3f : 0.2f;
      float gr = ImMin(col2.x + boost, 1.0f);
      float gg = ImMin(col2.y + boost, 1.0f);
      float gb = ImMin(col2.z + boost, 1.0f);
      const int layers = 2;
      for (int i = layers; i >= 1; --i) {
        float expand = (float)i * 1.0f;
        float alpha = (0.10f / (float)i) * t;
        draw->AddRectFilled(
            ImVec2(rMin.x - expand, rMin.y - expand),
            ImVec2(rMax.x + expand, rMax.y + expand),
            ImGui::GetColorU32(ImVec4(gr, gg, gb, alpha)),
            1.5f + expand * 0.5f);
      }
    }
  }

  // Context menu for second color
  if ( ImGui::BeginPopupContextItem( "##ctx2" ) ) {
    ImGui::PushStyleVar( ImGuiStyleVar_ItemSpacing, ImVec2( 0, 2 ) );
    ImGui::PushStyleVar( ImGuiStyleVar_FramePadding, ImVec2( 8.0f, 4.0f ) );
    ImGui::PushStyleVar( ImGuiStyleVar_ButtonTextAlign, ImVec2( 0.5f, 0.7f ) );
    ImGui::PushStyleColor( ImGuiCol_Button, colors::comboBg );
    ImGui::PushStyleColor( ImGuiCol_ButtonHovered, ImVec4( 0.067f, 0.067f, 0.067f, 1.0f ) );
    ImGui::PushStyleColor( ImGuiCol_ButtonActive, ImVec4( 0.1f, 0.1f, 0.1f, 1.0f ) );
    if ( ImGui::Button( "Copy", ImVec2( 60.0f, 0 ) ) ) {
      copiedColor    = col2;
      hasColorCopied = true;
      ImGui::CloseCurrentPopup();
    }
    if ( ImGui::Button( "Paste", ImVec2( 60.0f, 0 ) ) ) {
      if ( hasColorCopied ) col2 = copiedColor;
      ImGui::CloseCurrentPopup();
    }
    ImGui::PopStyleColor( 3 );
    ImGui::PopStyleVar( 3 );
    ImGui::EndPopup();
  }

  // Popup for editing
  const bool isPopupOpen     = ImGui::IsPopupOpen( popupLabel.c_str() );
  const bool anchorVisible   = IsPopupAnchorVisibleInCurrentWindow( popupAnchorMin[stateId], popupAnchorMax[stateId] );
  const bool canStartBelow  = CanPopupStartBelowCurrentWindow( popupAnchorMax[stateId] );
  const bool shouldAutoClose = isPopupOpen && ( !anchorVisible || !canStartBelow );

  auto&   motion     = WindowMotion::System();
  const auto& tokens = WindowMotion::Tokens();
  float popupAlpha   = motion.value( ui::motion::MotionKey( "color-popup", std::to_string( static_cast<ImU32>( stateId ) ), "alpha" ), isPopupOpen ? 1.0f : 0.0f, isPopupOpen ? tokens.fadeFast : tokens.fadeFast, 0.0f );
  float popupSlide   = motion.value( ui::motion::MotionKey( "color-popup", std::to_string( static_cast<ImU32>( stateId ) ), "slide" ), isPopupOpen ? 1.0f : 0.0f, isPopupOpen ? tokens.springSnappy : tokens.fadeFast, 0.0f );

  if ( isPopupOpen ) {
    if ( popupAnchorMin.find( stateId ) != popupAnchorMin.end() && popupAnchorMax.find( stateId ) != popupAnchorMax.end() ) {
      const ImVec2 basePopupPos = GetAnchoredColorPopupPos( popupAnchorMin[stateId], popupAnchorMax[stateId] );
      const ImVec2 animatedPopupPos( basePopupPos.x, basePopupPos.y + ( 1.0f - popupSlide ) * 6.0f );
      ImGui::SetNextWindowPos( animatedPopupPos );
    }
  }

  // v2 popup styling
  ImGui::PushStyleColor( ImGuiCol_PopupBg, colors::comboBg );
  ImGui::PushStyleColor( ImGuiCol_Border, colors::border );
  ImGui::PushStyleVar( ImGuiStyleVar_PopupBorderSize, 1.0f );

  if ( ImGui::BeginPopup( popupLabel.c_str() ) ) {
    if ( shouldAutoClose ) {
      WindowMotion::System().set( ui::motion::MotionKey( "color-popup", std::to_string( static_cast<ImU32>( stateId ) ), "alpha" ), 0.0f );
      WindowMotion::System().set( ui::motion::MotionKey( "color-popup", std::to_string( static_cast<ImU32>( stateId ) ), "slide" ), 0.0f );
      popupJustOpened[stateId] = false;
      ImGui::CloseCurrentPopup();
      ImGui::EndPopup();
      ImGui::PopStyleVar();
      ImGui::PopStyleColor( 2 );
      ImGui::PopID();
      return;
    }

    ImGui::PushStyleVar( ImGuiStyleVar_Alpha, popupAlpha );
    ImGui::PushStyleVar( ImGuiStyleVar_WindowPadding, ImVec2( 6.0f, 6.0f ) );
    ImGui::PushStyleVar( ImGuiStyleVar_ItemSpacing, ImVec2( 6.0f, 2.0f ) );

    ImVec4& activeCol = ( editIndex == 1 ) ? col2 : col1;

    const float picker_w = GetAdvancedColorPickerWidth();
    ImGui::SetNextItemWidth( picker_w );
    bool colorChanged = useAlpha
                            ? ImGui::ColorPicker4( "##picker", (float*)&activeCol, ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoSidePreview | ImGuiColorEditFlags_NoOptions | ImGuiColorEditFlags_PickerHueBar | ImGuiColorEditFlags_InputRGB )
                            : ImGui::ColorPicker3( "##picker", (float*)&activeCol, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoSidePreview | ImGuiColorEditFlags_NoOptions | ImGuiColorEditFlags_PickerHueBar | ImGuiColorEditFlags_InputRGB );

    const float full_w = picker_w;

    ImGui::Dummy( ImVec2( 0.0f, 1.0f ) );

    ImGui::PushStyleVar( ImGuiStyleVar_FramePadding, ImVec2( 5.0f, 3.0f ) );
    colorChanged |= RenderAdvancedColorValueEditor( activeCol, useAlpha, full_w );
    ImGui::PopStyleVar();

    if ( RenderColorPalette( activeCol ) ) {
      colorChanged = true;
    }

    // Animation removed to fix snap for color palette

    // OK button removed as requested


    // Pop compact style + alpha.
    ImGui::PopStyleVar( 3 );
    ImGui::EndPopup();
  } else {
    if ( popupJustOpened.find( stateId ) != popupJustOpened.end() && popupJustOpened[stateId] ) {
      popupJustOpened[stateId] = false;
    }
  }
  ImGui::PopStyleVar();
  ImGui::PopStyleColor( 2 );

  ImGui::PopID();
}

inline void CustomDualColorEdit4Sized( const char* label, ImVec4& col1, ImVec4& col2, float size ) {
  CustomDualColorEditSized( label, col1, col2, true, size );
}

inline void CustomDualColorEdit3Sized( const char* label, ImVec4& col1, ImVec4& col2, float size ) {
  CustomDualColorEditSized( label, col1, col2, false, size );
}

// Custom combo widget with simplified styling
inline bool CustomCombo( const char* label, int* current_item, const std::vector<std::string>& items ) {
  // Safety checks
  if ( items.empty() || !current_item || *current_item < 0 || *current_item >= static_cast<int>( items.size() ) )
    return false;

  bool value_changed = false;

  // Match the menu alpha used elsewhere for a consistent fade-in / fade-out effect
  float  menuAlpha    = globals::overlayVisible ? 1.0f : 0.0f;
  ImVec4 frameBgColor = ImGui::GetStyleColorVec4( ImGuiCol_Button );
  frameBgColor.w *= menuAlpha;

  // Frame background colouring (drop down closed)
  ImGui::PushStyleColor( ImGuiCol_FrameBg, frameBgColor );
  // Minimal vertical padding similar to Hotkey widget
  ImGui::PushStyleVar( ImGuiStyleVar_FramePadding, ImVec2( ImGui::GetStyle().FramePadding.x, 5.f ) );

  const char* preview_value = items[*current_item].c_str();
  // Calculate the width of the label to align the combo box
  float label_width = ImGui::CalcTextSize( label ).x;
  ImGui::PushItemWidth( ImGui::GetContentRegionAvail().x );
  if ( ImGui::BeginCombo( label, preview_value, ImGuiComboFlags_PopupAlignLeft ) ) {
    // Iterate items
    for ( int i = 0; i < static_cast<int>( items.size() ); ++i ) {
      bool        is_selected  = ( *current_item == i );
      const char* item_label   = items[i].c_str();
      ImVec4      normalItemBg = ImGui::GetStyleColorVec4( ImGuiCol_FrameBg );
      normalItemBg.w *= menuAlpha;

      ImVec4 selectedItemBg = LightenColor( normalItemBg, 0.08f );  // Adjust factor for selected item
      ImVec4 hoveredItemBg  = LightenColor( normalItemBg, 0.15f );  // Adjust factor for hovered item
      ImVec4 activeItemBg   = LightenColor( normalItemBg, 0.10f );  // Adjust factor for active item (when clicked)

      ImGui::PushStyleColor( ImGuiCol_Header, is_selected ? selectedItemBg : normalItemBg );
      ImGui::PushStyleColor( ImGuiCol_HeaderHovered, hoveredItemBg );
      ImGui::PushStyleColor( ImGuiCol_HeaderActive, activeItemBg );
      ImGui::PushStyleVar( ImGuiStyleVar_FramePadding, ImVec2( ImGui::GetStyle().FramePadding.x, 0.f ) );

      if ( ImGui::Selectable( item_label, is_selected ) ) {
        *current_item = i;
        value_changed = true;
      }
      if ( is_selected )
        ImGui::SetItemDefaultFocus();

      ImGui::PopStyleVar();
      ImGui::PopStyleColor( 3 );
    }
    ImGui::EndCombo();
  }

  ImGui::PopItemWidth();
  ImGui::PopStyleVar();
  ImGui::PopStyleColor();

  return value_changed;
}

// Overload that matches ImGui::CustomCombo(label,&idx,names.data(),namesCount)
namespace ImGui {
  // Forwarding overload for std::vector<std::string>
  inline bool CustomCombo( const char* label, int* current_item, const std::vector<std::string>& items ) {
    return ::CustomCombo( label, current_item, items );
  }

  inline bool CustomCombo( const char* label, int* current_item, const char** items, int items_count ) {
    if ( !items || items_count <= 0 )
      return false;
    std::vector<std::string> vec;
    vec.reserve( items_count );
    for ( int i = 0; i < items_count; ++i )
      vec.emplace_back( items[i] );
    return ::CustomCombo( label, current_item, vec );
  }

}  // namespace ImGui

// Custom animated checkbox function (now just calls the standard ImGui::Checkbox which has animations built-in)
inline bool CustomCheckbox( const char* label, bool* v ) {
  // Since we've modified the standard ImGui::Checkbox to include animations,
  // this function now just calls the standard checkbox
  return ImGui::Checkbox( label, v );
}

namespace ImGui {
  inline bool Hotkey( int* k, HotkeyMode* mode = nullptr, const ImVec2& size_arg = ImVec2( 0, 0 ), bool allowAlways = true, bool allowOff = true ) {
    bool changed = false;
    ImGui::PushID( k );

    ImGuiID       id              = ImGui::GetID( (void*)k );
    ImGuiStorage* storage         = ImGui::GetStateStorage();
    bool          waiting_for_key = storage->GetBool( id, false );

    std::string keyText;
    if ( *k == 0 ) {
      keyText = "None"; 
    } else {
      if ( KeyNames.count( *k ) ) {
        keyText = KeyNames.at( *k ); 
      } else {
        keyText = "None"; 
      }
    }

    // Display text changes to "..." while waiting for key input
    std::string displayText = waiting_for_key ? "..." : keyText;

    ImFont* font = menuFont_px11 ? menuFont_px11 : ImGui::GetFont();
    float fontSize = font->FontSize;
    ImVec2 textSize = font->CalcTextSizeA( fontSize, FLT_MAX, 0.0f, displayText.c_str() );
    float targetW = ImMax( textSize.x + 12.0f, 36.0f );

    auto& motion = WindowMotion::System();
    const auto& tokens = WindowMotion::Tokens();

    ImVec2 size = size_arg;
    if ( size.x == 0.0f ) {
      size.x = motion.value( ui::motion::MotionKey( "hotkey", std::to_string( static_cast<ImU32>( id ) ), "width" ), targetW, tokens.slideSoft, targetW );
    }
    if ( size.y == 0.0f ) size.y = 18.0f;

    bool pressed = ImGui::InvisibleButton( "##hotkey", size );

    if ( !waiting_for_key ) {
      if ( pressed ) {
        storage->SetBool( id, true );
      }
    } else {
        bool mouse_down = ImGui::IsMouseDown( 0 ) || ImGui::IsMouseDown( 1 ) || ImGui::IsMouseDown( 2 );
        
        // Use a static or storage variable to track if we've released the activation button
        ImGuiID releasedId = ImGui::GetID( "hotkey_released" );
        if ( !storage->GetBool( releasedId, false ) ) {
            if ( !mouse_down ) {
                storage->SetBool( releasedId, true );
            }
        } else {
            for ( int key : KeyCodes ) {
                if ( key == 0 ) continue;
                
                if ( GetAsyncKeyState( key ) & 0x8000 ) {
                    if ( key == 0x1B ) { // Escape
                        *k = 0;
                    } else {
                        *k = key;
                    }
                    storage->SetBool( id, false );
                    storage->SetBool( releasedId, false ); // Reset for next time
                    waiting_for_key = false;
                    break;
                }
            }
        }
        
        // Allow clicking away to cancel
        if ( ImGui::IsMouseClicked( 0 ) && !ImGui::IsItemHovered() ) {
            storage->SetBool( id, false );
            storage->SetBool( releasedId, false );
            waiting_for_key = false;
        }
    }

    // Custom Drawing
    ImDrawList* draw = ImGui::GetWindowDrawList();
    ImVec2 pMin = ImGui::GetItemRectMin();
    ImVec2 pMax = ImGui::GetItemRectMax();
    ImVec2 center = ImVec2((pMin.x + pMax.x) * 0.5f, (pMin.y + pMax.y) * 0.5f);
    const bool hovered = ImGui::IsItemHovered();

    const float hoverT = motion.value( ui::motion::MotionKey( "hotkey", std::to_string( static_cast<ImU32>( id ) ), "hover" ), hovered ? 1.0f : 0.0f, tokens.hoverFast, hovered ? 1.0f : 0.0f );
    const float waitT  = motion.value( ui::motion::MotionKey( "hotkey", std::to_string( static_cast<ImU32>( id ) ), "waiting" ), waiting_for_key ? 1.0f : 0.0f, tokens.pressFast, waiting_for_key ? 1.0f : 0.0f );

    const ImVec4 borderBase = colors::border;
    const ImVec4 borderHover = ImVec4(0.267f, 0.267f, 0.267f, 1.0f);
    const ImVec4 borderWait = ImVec4(0.267f, 0.267f, 0.267f, 1.0f);
    const ImVec4 borderAfterHover(
        borderBase.x + (borderHover.x - borderBase.x) * hoverT,
        borderBase.y + (borderHover.y - borderBase.y) * hoverT,
        borderBase.z + (borderHover.z - borderBase.z) * hoverT,
        borderBase.w + (borderHover.w - borderBase.w) * hoverT
    );
    const ImVec4 borderFinal(
        borderAfterHover.x + (borderWait.x - borderAfterHover.x) * waitT,
        borderAfterHover.y + (borderWait.y - borderAfterHover.y) * waitT,
        borderAfterHover.z + (borderWait.z - borderAfterHover.z) * waitT,
        borderAfterHover.w + (borderWait.w - borderAfterHover.w) * waitT
    );
    draw->AddRect(pMin, pMax, ImGui::GetColorU32(borderFinal));
    
    // Text
    float textHeight = font->Ascent - font->Descent; 
    
    const ImVec4 textBase = colors::text;
    const ImVec4 textWait = ImVec4(1.0f, 0.0f, 0.0f, 1.0f);
    const ImVec4 textFinal(
        textBase.x + (textWait.x - textBase.x) * waitT,
        textBase.y + (textWait.y - textBase.y) * waitT,
        textBase.z + (textWait.z - textBase.z) * waitT,
        textBase.w + (textWait.w - textBase.w) * waitT
    );
    ImU32 textColor = ImGui::GetColorU32(textFinal);

    textSize = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, displayText.c_str());
    
    // Centering – clip text to the animated box so it doesn't overflow during resize
    float textX = floorf(center.x - textSize.x * 0.5f + 0.5f);
    float textY = floorf(center.y - textHeight * 0.5f + 0.5f);
    
    draw->PushClipRect(pMin, pMax, true);
    draw->AddText(font, fontSize, ImVec2(textX, textY), textColor, displayText.c_str());
    draw->PopClipRect();

    if ( mode != nullptr ) {
        if ( !allowAlways && *mode == AlwaysOn ) {
          *mode = Toggle;
          changed = true;
        }
        if ( !allowOff && *mode == Disabled ) {
          *mode = Toggle;
          changed = true;
        }

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 8.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 1.0f);
        ImGui::PushStyleColor(ImGuiCol_PopupBg, colors::comboBg);
        ImGui::PushStyleColor(ImGuiCol_Border, colors::border);

        if ( ImGui::BeginPopupContextItem() ) {
            ImDrawList* popupDraw = ImGui::GetWindowDrawList();
            ImVec2 popupMin = ImGui::GetWindowPos();
            ImVec2 popupMax = ImVec2(popupMin.x + ImGui::GetWindowSize().x, popupMin.y + ImGui::GetWindowSize().y);
            popupDraw->AddRectFilled(popupMin, popupMax, ImGui::GetColorU32(colors::comboBg));

            if ( allowAlways ) {
              if ( CustomRadioButton( "Always", (int*)mode, AlwaysOn ) ) {
                changed = true;
              }
            }
            if ( CustomRadioButton( "Toggle", (int*)mode, Toggle ) ) {
              changed = true;
            }
            if ( CustomRadioButton( "Hold", (int*)mode, Hold ) ) {
              changed = true;
            }
            if ( allowOff ) {
              if ( CustomRadioButton( "Off", (int*)mode, Disabled ) ) {
                changed = true;
              }
            }
            ImGui::EndPopup();
        }
        
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar(2);
    }

    ImGui::PopID();
    return changed;
  }
}  // namespace ImGui

// End of imgui_extra.h
