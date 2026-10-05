<div align="center">

<img src="images/hypr3d.png" width="96" height="96" alt="лого">

# Hypr3D =^..^=

**A new perspective on window management - literally.**
A Hyprland plugin that turns your workspace into a walkable 3D space

[Installation](#installation) · [Use](#use) · [Configuration](#configuration) · [Report a bug / suggest a new feature](https://github.com/samine825/Hypr3D/issues)

![alt text](images/Screenshot1.png)
</div>

# <a name="installation"></a> Installation
## Hyprpm

```bash
# Install latest
hyprpm add https://github.com/samine825/Hypr3D
hyprpm enable Hypr3D

# Update
hyprpm update
```



## Manual

```bash
# Build
cmake -S . -B build -DHYPRLAND_HEADERS=/var/cache/hyprpm/$USER/headersRoot
cmake --build build -j$(nproc)

# Load
hyprctl plugin load "$PWD/build/hypr3d.so"
```

# <a name="use"></a> Use

To enable 3D:

```bash
hyprctl eval 'hl.plugin.hypr3d.toggle()'
```

Or bind in lua config:

```lua
hl.bind("SUPER + F12", hl.plugin.hypr3d.toggle)
```

## Independent 2D / 3D positions

Window positions are per-mode. The 2D desktop keeps its own layout: ghosting
saves every window's geometry on entry and restores it exactly on exit, and
dragging or resizing a window inside the room never disturbs it (a floating
window keeps a 3D resize, a tiled one returns to its tile). The room, in
turn, remembers where every window was dragged: on the next toggle each
window comes back to its own 3D spot instead of spawning at the default
place in front of you.

The toggle itself is not a fade. Every window's 2D rectangle is
back-projected onto the camera frustum plane -- the pose where the quad
covers its on-screen spot exactly (the same construction the fullscreen
passthrough uses) -- and the quads take off from there and fly to their
room poses while the room itself materializes around them; leaving 3D
reverses it. Neither handoff shows a jump: on entry the first 3D frame is
pixel-identical to the 2D desktop, and on exit the last 3D frame is
pixel-identical to what the compositor is about to show. Windows whose 2D
box differs from their room box (tiled windows, which the ghosting
force-floats) animate their real box along the way, so the client re-renders
mid-flight and the content never pops scale. The player keeps their walk
across toggles too: position and look angles are remembered, so re-entering
the room continues where you left off instead of resetting to the spawn.

## Controls

| Input                         | Action                                         |
| -------------------------------| ------------------------------------------------|
| Mouse move                    | Look around                                    |
| WASD                          | Move                                           |
| Space                         | Move up (flying) / Jump                        |
| Shift                         | Move down (flying)                             |
| Ctrl                          | Sprint                                         |
| C                             | Zoom, wheel adjusts                            |
| Super + Left click            | Drag window                                    |
| Super + Right click           | Resize window                                  |
| Super + Mouse wheel click     | rotate window                                  |
| Super + Mouse wheel scrolling | Zoom window                                    |
| Super + Left Alt              | Toggle keyboard mode (movement / window input) |
| F3                            | Toggle debug HUD                               |
| F5                            | Switch camera view                             |

# <a name="configuration"></a> Configuration

## Lua config example:

```lua
if hl.plugin.hypr3d then
    hl.bind("SUPER + F12", hl.plugin.hypr3d.toggle)
    hl.plugin.hypr3d.config({
        world = {
            panorama = "~/panorama.png",
            grid = false,
        },
        windows = {
            window_scale = 0.25,
            spawn_distance = 2,
        },
        player = {
            mesh = {
                path = "~/player.gltf",
                transform = {
                    rotation = {0, 90, 0},
                    scale =    {1.5, 1.5, 1.5},
                },
                emissive_scale = 1.0,
                flat = true,
                center = "origin",
            },
            animations = {
                idle = {source = "Idle"},
                walk = {source = "Walking"},
                run = {source = 2},
                jump = {source = 3, duration_scale=2.4},
            },
            collision = true,
            move_speed = 1.5,
            spawn = {0, 67, 0},
            flying = false,
            walk_bob = true
        },
        scene = {
            mymap = {
                mesh = {
                    path = "~/scene.gltf",
                    transform = {
                        position = {1, 0, 0},
                    },
                    emissive_scale = 1.0,
                    center_offset = {0, 34, 0},
                },
                collision = true,
                static = true,
            },
            anyname = {
                mesh = {
                    path = "eevee.gltf",
                    transform = {
                        position = {y = 3},
                        scale =    {2, 2, 2},
                    },
                    emissive_scale = 1.0,
                    flat = true,
                    center = "origin",
                    center_offset = {0, 20, 0},
                },
                collision = true,
                static = false,
                physics = true,
            },
        }
    })
end
```



## Parameters

Everything is optional -- only set what you want to change. 

### World

| Option   | Type   | Default | Description                             |
| ----------| --------| ---------| -----------------------------------------|
| panorama | string | ""      | 360° background image (equirectangular) |
| grid     | bool   | true    | the starting 40x40 platform             |
| monitor  | string | ""      | render the 3D view on this monitor (e.g. "DP-1") instead of the focused one |

### Windows

| Option         | Type  | Default | Description                             |
| ----------------| -------| ---------| -----------------------------------------|
| window_scale   | float | 0.5     | window size multiplier (at 100 px/m)    |
| spawn_distance | float | 5.0     | how far from you new windows appear (m) |
| depth          | float | 0.05    | window slab thickness in world units (0 = flat quads); the walls follow the window's rounded corners and are painted with the texture's edge colors |

### Player

| Option           | Type                                | Default     | Description                                             |
| ------------------| -------------------------------------| -------------| ---------------------------------------------------------|
| mesh             | [Mesh](#mesh)                       | {}          | object's visual                                         |
| animations       | [Animation Group](#animation-group) | {}          | which clip each movement state plays                    |
| look_sensitivity | float                               | 0.0025      | how fast the camera turns (radians per pointer count)   |
| look_inertia     | float                               | 0.03        | camera coasting after you stop the mouse (in seconds)   |
| move_inertia     | float                               | 0.05        | coasting after you stop walking in seconds (in seconds) |
| move_speed       | float                               | 4.0         | how fast you walk (m/s, running - 2.5x)                 |
| spawn            | [Vector3](#vector3)                 | { 0, 0, 0 } | player spawn point                                      |
| flying           | bool                                | true        | disables falling                                        |
| walk_bob         | bool                                | true        | simulate the rhythm of walking                          |
| collision        | bool                                | true        | whether body collides with the world                    |

### Scene

| Option          | Type                          | Default | Description                        |
| -----------------| -------------------------------| ---------| ------------------------------------|
| any unique name | [Scene Object](#scene-object) | {}      | u can place any number of objects. |

### <a name="scene-object"></a> Scene Object

| Option    | Type          | Default | Description                                          |
| -----------| ---------------| ---------| ------------------------------------------------------|
| mesh      | [Mesh](#mesh) | {}      | object's visual                                      |
| collision | bool          | true    | whether body collides with the world                 |
| static    | bool          | true    | you can't pick it up and carry it; suitable for maps |
| physics   | bool          | false   | enable jolt physics (only if static=false)           |

Want only part of a model to be solid? Rename those nodes in Blender to
start with `nocol` -- they'll still render, but you'll walk right through.

### <a name="mesh"></a> Mesh

| Option         | Type                    | Default   | Description                         |
| ----------------| -------------------------| -----------| -------------------------------------|
| path           | string                  | ""        | the model file to load (.glb/.gltf) |
| transform      | [Transform](#transform) | {}        | idk                                 |
| emissive_scale | float                   | 1.0       | how bright the model's own light is |
| flat           | bool                    | false     | trust the model's lighting as-is    |
| center         | string                  | logical   | origin / logical                    |
| center_offset  | [Vector3](#vector3)     | {0, 0, 0} | Extra shift of the rotation pivot   |

### <a name="animation-group"></a> Animation Group

| Option | Type                    | Default |
| --------| -------------------------| ---------|
| idle   | [Animation](#animation) | {}      |
| walk   | [Animation](#animation) | {}      |
| run    | [Animation](#animation) | {}      |
| jump   | [Animation](#animation) | {}      |

### <a name="animation"></a> Animation

| Option         | Type       | Default | Description                                                       |
| ----------------| ------------| ---------| -------------------------------------------------------------------|
| source         | int/string | {}      | which anim to play: its name in the file, or its zero-based index |
| duration_scale | float      | 1.0     | playback speed multiplier                                         |

### <a name="transform"></a> Transform

| Option   | Type                | Default   |
| ----------| ---------------------| -----------|
| position | [Vector3](#vector3) | {0, 0, 0} |
| rotation | [Vector3](#vector3) | {0, 0, 0} |
| scale    | [Vector3](#vector3) | {1, 1, 1} |

### <a name="vector3"></a> Vector3

Vectors can be written either way: `{ x = 1, y = 2, z = 3 }` or just `{ 1, 2, 3 }`.

# Screenshots
<table>
  <tr>
    <td align="center" width="25%"><img src="images/Screenshot5.png" alt="screenshot"></a></td>
    <td align="center" width="25%"><img src="images/Screenshot4.png" alt="screenshot"></a></td>
  </tr>
  <tr>
    <td align="center" width="25%"><img src="images/Screenshot2.png" alt="screenshot"></a></td>
    <td align="center" width="25%"><img src="images/Screenshot3.png" alt="Nscreenshot"></a></td>
  </tr>
<tr>
    <td align="center" width="25%"><img src="images/Screenshot6.png" alt="screenshot"></a></td>
    <td align="center" width="25%"><img src="images/Screenshot7.png" alt="screenshot"></a></td>
  </tr>
</table>
