---
name: macos-design
description: Authoritative Apple macOS Human Interface Guidelines (HIG) and premium design system specification for AI coding agents. Enforces strict zero-AI-slop principles, authentic window anatomy, materials, typography hierarchy, control ergonomics, and physical spring motion.
---

# macOS Premium Design System & HIG Specification for AI Agents

This specification defines the rigorous visual language, spatial ergonomics, material architecture, and motion physics required to build software that feels indistinguishable from premier first-party macOS software (such as macOS Settings, Finder, Xcode, and Apple Pro apps).

---

## 1. Core Philosophy: Clarity, Deference, Depth

1. **Clarity**: Text is crisp at every point size, icons are precise and purposeful, ornaments are subtle and functional, and critical features are immediately legible.
2. **Deference**: Content is king. The interface recedes gracefully into the background. Chrome and framing never compete with the user's data or settings.
3. **Depth**: Visual layers, translucency, and realistic elevation hierarchy impart vitality and facilitate spatial comprehension without superficial visual noise.

---

## 2. Forbidden AI Slop & Cliché Tropes (STRICT GUARDRAILS)

AI models frequently default to generic, cheap-looking "futuristic" web tropes. When crafting macOS-grade UI, **NEVER** use:

| Forbidden AI Slop Trope | Why It Fails | macOS HIG Standard |
| :--- | :--- | :--- |
| ❌ **Harsh Neon/Purple Glows** | Looks like a cheap gamer dashboard or crypto landing page. | Subtle 1px neutral specular borders (`#ffffff` at 6–12% opacity) + diffused ambient occlusion shadows. |
| ❌ **Over-saturated Accent Gradients** | Distracts the eye and breaks content deference. | Refined monochrome tones or Apple system tints (System Blue `#007AFF`, Slate, Zinc) used sparingly for primary actions. |
| ❌ **Nested Bento Boxes & Card-in-Card Clutter** | Creates visual claustrophobia and arbitrary visual breaks. | Flat grouped inset lists with 1px hair-thin dividers (`0.5pt` / 1px scaled at 6–10% white). |
| ❌ **Gigantic Trackless Headings** | Disproportionate hierarchy that wastes space. | Carefully tracked Inter/SF Pro typography (Semibold headers with negative tracking, Regular body with optical baseline centering). |
| ❌ **Gimmicky Floating Badges & Pulsing Dots** | Distracting animations that simulate fake importance. | Clean, solid or translucent status pills with calm, stable status indicators. |
| ❌ **Arbitrary Hover Animations / Wild Rotation** | Gimmicky and disorienting. | Damped spring transitions, subtle scale nudges (e.g. 0.98x press shrink, 1.02x soft lift), and 150–250ms smooth fades. |

---

## 3. Window Anatomy & Chrome

### 3.1 Window Structure
- **Corner Radius**: 12px to 14px continuous curvature (squircle/super-ellipse aesthetic).
- **Split-View / Sidebar**:
  - Sidebar width: 170px–220px.
  - Materials: Translucent frosted sidebar with subtle backdrop blur, visually separated by a 1px vertical hairline divider.
  - Navigation items: Inset pills with a sliding spring-animated selection pill (`rgba(255, 255, 255, 0.10)` on dark, `#ffffff` card on light).

### 3.2 Traffic Lights (Window Controls)
- **Geometry**: Three 12px circles placed at `(18px, 18px)` from the top-left window origin.
- **Spacing**: 8px gap between dots (centers spaced at 20px).
- **Colors**:
  - Close: `#FF5F57` (border `#E0443E`)
  - Minimize: `#FEBC2E` (border `#D89E24`)
  - Zoom / Maximize: `#28C840` (border `#1AAB29`)
  - *Inactive/Unfocused Window*: Uniform muted zinc (`#3A3A3C`).
- **Hover Behavior**:
  - Glyph icons appear smoothly on hover (X for close, - for minimize, + or bidirectional diagonal arrow for zoom).
  - Micro-scale bounce or subtle tint intensification.

### 3.3 Titlebar & Dragging
- Unified titlebar integrated seamlessly into the window background without heavy horizontal separator bars.
- Full support for native window dragging and snap layouts across unoccupied titlebar/sidebar regions.

---

## 4. Materials & Color System (Dark Mode Focus)

macOS achieves its signature premium look through layered, dark neutral tones and high dynamic contrast:

```
┌─────────────────────────────────────────────────────────┐
│ Window Background (Ultra-dark Zinc / Base: #0B0B0C)     │
│  ┌──────────────────┐  ┌──────────────────────────────┐ │
│  │ Sidebar (#0E0E10)│  │ Content Pane (#141416)       │ │
│  │                  │  │  ┌────────────────────────┐  │ │
│  │ Active Pill:     │  │  │ Grouped Card (#1C1C1E) │  │ │
│  │ rgba(255,255,255,│  │  │ 1px Border:            │  │ │
│  │      0.10)       │  │  │ rgba(255,255,255,0.08) │  │ │
│  │                  │  │  └────────────────────────┘  │ │
│  └──────────────────┘  └──────────────────────────────┘ │
└─────────────────────────────────────────────────────────┘
```

- **Base Window**: `#0B0B0D` (Deep charcoal, warm black)
- **Sidebar**: `#0E0E10` (80% opacity with backdrop blur)
- **Content Canvas**: `#131315`
- **Grouped Card / Inset Container**: `#1C1C1E` (Subtle 14px rounded container)
- **Card Border**: `rgba(255, 255, 255, 0.08)` to `rgba(255, 255, 255, 0.12)`
- **Hairline Separators**: `rgba(255, 255, 255, 0.06)`
- **Primary Text**: `#FFFFFF` (96% white, crisp readability)
- **Secondary / Muted Text**: `#8E8E93` (Apple System Gray)
- **Accent / Interactive**: `#007AFF` or Cool High-Contrast White/Silver (`#E8E8EC`)

---

## 5. Typography & Spatial Rhythm

### 5.1 Typography Scale (SF Pro / Inter)
- **Window Title / Hero**: 18pt–20pt, Semibold, -0.015em letter-spacing.
- **Section Headers**: 13pt–14pt, Semibold, crisp tracking.
- **Body & Row Labels**: 13pt, Regular, line height 1.35.
- **Captions & Secondary Descriptions**: 11pt–12pt, Regular, muted tone (`#8E8E93`).
- **Footnotes & Monospace Keys**: 10pt–11pt, Medium.

### 5.2 Layout Grid & Padding
- **Outer Page Inset**: 24px–28px.
- **Grouped Card Inner Inset**: 14px–16px.
- **Vertical Row Height**:
  - Compact Row: 38px
  - Standard Action Row: 46px–48px
  - Hero / Rich Row: 60px–72px
- **Inter-Card Spacing**: 16px–20px.

---

## 6. Native Controls & Interaction Patterns

### 6.1 Switches / Toggles
- Rounded pill track: 40px × 24px.
- Smooth sliding thumb (white circle 20px diameter with 2px padding).
- Animated track color transition from neutral off (`#2C2C2E`) to active on (`#34C759` or `#007AFF` or `#E8E8EC`).

### 6.2 Segmented Controls
- Continuous rounded background tray with 6px–8px radius.
- Sliding selected pill on a physical spring with soft elevation shadow (`rgba(0,0,0,0.25)`).
- Instant, crisp text contrast shift on selection.

### 6.3 Sliders & Scrubbers
- 4px–6px sleek rounded track.
- Filled progress bar with smooth gradient or active accent.
- Circular or pill grab handle (16px–18px) that scales up slightly (`+10%`) on active drag.

### 6.4 Buttons
- **Primary Button**: Solid filled pill with subtle top specular highlight, smooth active press shrink (`0.95x`).
- **Secondary / Outline Button**: Translucent background (`rgba(255,255,255,0.06)`) with 1px hairline border.
- **Destructive Button**: Crimson red text or muted red background.

---

## 7. Motion & Physics (Spring Dynamics)

Every animation in macOS feels rooted in Newtonian physics:

1. **Spring Stiffness & Damping**:
   - `Spring Soft`: Damping ratio ~0.85, mass 1.0, stiffness ~220. Smooth for sliding nav pills and segmented indicators.
   - `Spring Snappy`: Damping ratio ~0.80, stiffness ~380. Immediate for toggles and button presses.
2. **Staggered Orchestration**:
   - Sequential page elements enter with an intentional 25–40ms stagger.
   - Initial offset is gentle (4px–8px Y-shift), avoiding disorienting 50px jumps.
3. **Interruptibility**:
   - Any active animation must seamlessly blend into new input without stutter or positional reset.
