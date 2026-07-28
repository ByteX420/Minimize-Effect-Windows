cbuffer GenieConstants : register(b0) {
  float4 source;
  float4 target;
  float progress;
  float strength;
  uint edge;
  uint style;
};
cbuffer VisualConstants : register(b2) {
  float2 texture_size;
  float shadow_radius;
  float shadow_opacity;
  uint render_shadow;
  float animation_progress;
  uint has_per_pixel_alpha;
  uint texture_rotation;
};

struct VertexInput {
  float2 texcoord : TEXCOORD0;
};
struct PixelInput {
  float4 position : SV_POSITION;
  float2 texcoord : TEXCOORD0;
};

static const uint EDGE_TOP = 0;
static const uint EDGE_BOTTOM = 1;
static const uint EDGE_LEFT = 2;
static const uint EDGE_RIGHT = 3;
static const uint STYLE_CLASSIC = 0;
static const uint STYLE_CURVY = 1;
static const float PI = 3.14159265358979323846f;

float safe_divisor(float value) {
  if (abs(value) < 0.000001f) return value < 0.0f ? -0.000001f : 0.000001f;
  return value;
}

float quadratic_ease_in_out(float value) {
  float p = saturate(value);
  if (p < 0.5f) return 2.0f * p * p;
  return (-2.0f * p * p) + (4.0f * p) - 1.0f;
}

float sine_ease_in_out(float value) {
  return 0.5f - 0.5f * cos(saturate(value) * PI);
}

float curve_position(float position, float curve_start, float curve_end,
                     float value_start, float value_end) {
  if (position < curve_start) return value_start;
  if (position < curve_end) {
    float p = quadratic_ease_in_out(
        (position - curve_start) / safe_divisor(curve_end - curve_start));
    return lerp(value_start, value_end, p);
  }
  return value_end;
}

float2 linear_position(float2 uv) {
  float2 source_point = float2(lerp(source.x, source.z, uv.x),
                               lerp(source.y, source.w, uv.y));
  float2 target_point = float2(lerp(target.x, target.z, uv.x),
                               lerp(target.y, target.w, uv.y));
  return lerp(source_point, target_point, saturate(progress));
}

float2 classic_position(float2 uv) {
  float slide_progress = saturate(progress / 0.5f);
  float translate_progress = saturate((progress - 0.4f) / 0.6f);

  if (edge == EDGE_BOTTOM) {
    float vertical_distance = target.y - source.y;
    float translation = translate_progress * vertical_distance;
    float top_edge = min(source.w + translation, target.w);
    float bottom_edge = source.y + translation;
    float y = lerp(bottom_edge, top_edge, uv.y);
    float left_end = source.x + slide_progress * (target.x - source.x);
    float right_end = source.z + slide_progress * (target.z - source.z);
    return float2(
        lerp(curve_position(y, source.y, target.y, source.x, left_end),
             curve_position(y, source.y, target.y, source.z, right_end), uv.x),
        y);
  }

  if (edge == EDGE_TOP) {
    float vertical_distance = target.w - source.w;
    float translation = translate_progress * vertical_distance;
    float top_edge = source.w + translation;
    float bottom_edge = max(source.y + translation, target.y);
    float y = lerp(bottom_edge, top_edge, uv.y);
    float left_start = source.x + slide_progress * (target.x - source.x);
    float right_start = source.z + slide_progress * (target.z - source.z);
    return float2(
        lerp(curve_position(y, target.w, source.w, left_start, source.x),
             curve_position(y, target.w, source.w, right_start, source.z), uv.x),
        y);
  }

  if (edge == EDGE_LEFT) {
    float horizontal_distance = target.x - source.x;
    float translation = translate_progress * horizontal_distance;
    float left_edge = source.x + translation;
    float right_edge = min(source.z + translation, target.z);
    float x = lerp(left_edge, right_edge, uv.x);
    float top_end = source.w + slide_progress * (target.w - source.w);
    float bottom_end = source.y + slide_progress * (target.y - source.y);
    float y_bottom = curve_position(x, source.x, target.x, source.y, bottom_end);
    float y_top = curve_position(x, source.x, target.x, source.w, top_end);
    return float2(x, lerp(y_bottom, y_top, uv.y));
  }

  float horizontal_distance = target.z - source.z;
  float translation = translate_progress * horizontal_distance;
  float left_edge = max(source.x + translation, target.x);
  float right_edge = source.z + translation;
  float x = lerp(left_edge, right_edge, uv.x);
  float top_start = source.w + slide_progress * (target.w - source.w);
  float bottom_start = source.y + slide_progress * (target.y - source.y);
  float y_bottom = curve_position(x, target.z, source.z, bottom_start, source.y);
  float y_top = curve_position(x, target.z, source.z, top_start, source.w);
  return float2(x, lerp(y_bottom, y_top, uv.y));
}

float2 curvy_position(float2 uv) {
  bool horizontal = edge == EDGE_TOP || edge == EDGE_BOTTOM;
  float moving_extent = horizontal ? source.w - source.y : source.z - source.x;
  float distance_to_target;
  if (edge == EDGE_TOP) distance_to_target = source.w - target.w;
  else if (edge == EDGE_BOTTOM) distance_to_target = target.y - source.y;
  else if (edge == EDGE_LEFT) distance_to_target = source.z - target.z;
  else distance_to_target = target.x - source.x;
  distance_to_target = max(distance_to_target, 0.000001f);

  float shape_factor = clamp(max(0.20f, moving_extent / distance_to_target), 0.20f, 1.0f);
  float stretch_weight = 0.70f * shape_factor;
  float stretch_end = stretch_weight / (1.0f + stretch_weight);
  float stretch_progress = 0.0f;
  float squash_progress = 0.0f;
  if (progress < stretch_end) {
    stretch_progress = shape_factor * saturate(progress / safe_divisor(stretch_end));
  } else {
    squash_progress = saturate((progress - stretch_end) / safe_divisor(1.0f - stretch_end));
    stretch_progress = min(shape_factor + squash_progress, 1.0f);
  }

  float2 source_point = float2(lerp(source.x, source.z, uv.x),
                               lerp(source.y, source.w, uv.y));
  float2 target_point = float2(lerp(target.x, target.z, uv.x),
                               lerp(target.y, target.w, uv.y));
  if (horizontal) {
    float local = source_point.y - source.y;
    float offset = edge == EDGE_BOTTOM
                       ? local + distance_to_target * squash_progress
                       : local - distance_to_target * squash_progress;
    float shape_position = edge == EDGE_BOTTOM
                               ? offset / distance_to_target
                               : ((source.w - source.y) - offset) / distance_to_target;
    float scale = stretch_progress * sine_ease_in_out(shape_position);
    return float2(lerp(source_point.x, target_point.x, scale), source.y + offset);
  }

  float local = source_point.x - source.x;
  float offset = edge == EDGE_RIGHT
                     ? local + distance_to_target * squash_progress
                     : local - distance_to_target * squash_progress;
  float shape_position = edge == EDGE_RIGHT
                             ? offset / distance_to_target
                             : ((source.z - source.x) - offset) / distance_to_target;
  float scale = stretch_progress * sine_ease_in_out(shape_position);
  return float2(source.x + offset, lerp(source_point.y, target_point.y, scale));
}

PixelInput VertexMain(VertexInput input) {
  PixelInput output;
  float shadow_progress = 1.0f - saturate(progress);
  float2 shadow_extension =
      (2.0f * shadow_radius * shadow_progress) / max(texture_size, float2(1.0f, 1.0f));
  float2 shape_texcoord = render_shadow != 0
                              ? lerp(-shadow_extension, 1.0f + shadow_extension, input.texcoord)
                              : input.texcoord;
  float2 base_position = linear_position(shape_texcoord);
  float2 deformed = base_position;
  if (style == STYLE_CLASSIC) {
    deformed = lerp(base_position, classic_position(shape_texcoord), saturate(strength));
  } else if (style == STYLE_CURVY) {
    deformed = curvy_position(shape_texcoord);
  }
  output.position = float4(deformed.x * 2.0f - 1.0f, 1.0f - deformed.y * 2.0f, 0.0f, 1.0f);
  output.texcoord = shape_texcoord;
  return output;
}

cbuffer PixelConstants : register(b1) {
  float opacity;
  float3 padding;
};
Texture2D source_texture : register(t0);
Texture2D mask_texture : register(t1);
SamplerState linear_sampler : register(s0);
SamplerState mask_sampler : register(s1);

float shape_alpha(float2 texcoord) {
  return mask_texture.Sample(mask_sampler, texcoord).r;
}

float2 source_texcoord(float2 texcoord) {
  if (texture_rotation == 1) return float2(texcoord.y, 1.0f - texcoord.x);
  if (texture_rotation == 2) return 1.0f - texcoord;
  if (texture_rotation == 3) return float2(1.0f - texcoord.y, texcoord.x);
  return texcoord;
}

float4 PixelMain(float4 position : SV_POSITION, float2 texcoord : TEXCOORD0) : SV_TARGET {
  if (render_shadow != 0) {
    float remaining = 1.0f - saturate(animation_progress);
    float2 radius = (shadow_radius * remaining) / max(texture_size, float2(1.0f, 1.0f));
    float alpha = shape_alpha(texcoord) * 0.20f;
    alpha += (shape_alpha(texcoord + float2(radius.x * 0.33f, 0.0f)) +
              shape_alpha(texcoord - float2(radius.x * 0.33f, 0.0f)) +
              shape_alpha(texcoord + float2(0.0f, radius.y * 0.33f)) +
              shape_alpha(texcoord - float2(0.0f, radius.y * 0.33f))) * 0.10f;
    alpha += (shape_alpha(texcoord + float2(radius.x * 0.66f, 0.0f)) +
              shape_alpha(texcoord - float2(radius.x * 0.66f, 0.0f)) +
              shape_alpha(texcoord + float2(0.0f, radius.y * 0.66f)) +
              shape_alpha(texcoord - float2(0.0f, radius.y * 0.66f))) * 0.05f;
    float2 diagonal = radius * 0.47f;
    alpha += (shape_alpha(texcoord + diagonal) + shape_alpha(texcoord - diagonal) +
              shape_alpha(texcoord + float2(diagonal.x, -diagonal.y)) +
              shape_alpha(texcoord + float2(-diagonal.x, diagonal.y))) * 0.05f;
    alpha *= shadow_opacity * pow(remaining, 1.35f);
    return float4(0.0f, 0.0f, 0.0f, alpha);
  }
  float4 color = source_texture.Sample(linear_sampler, source_texcoord(texcoord));
  float mask_alpha = mask_texture.Sample(mask_sampler, texcoord).r;
  if (has_per_pixel_alpha != 0) {
    float shape_factor = color.a > 0.0001f ? saturate(mask_alpha / color.a) : 0.0f;
    color.rgb *= shape_factor * opacity;
    color.a = mask_alpha * opacity;
  } else {
    color.a *= mask_alpha * opacity;
    color.rgb *= color.a;
  }
  return color;
}
