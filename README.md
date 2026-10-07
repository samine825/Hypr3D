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
hyprpm enable hypr3d

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

## Controls

| Input                         | Action                                                   |
| -------------------------------| ----------------------------------------------------------|
| Mouse move                    | Look around                                              |
| WASD                          | Move                                                     |
| Space                         | Move up (flying) / Jump                                  |
| Shift                         | Move down (flying)                                       |
| Ctrl                          | Sprint                                                   |
| C                             | Zoom, wheel adjusts                                      |
| Super + Left click            | Drag window                                              |
| Super + Right click           | Resize window                                            |
| Super + Mouse wheel click     | rotate window                                            |
| Super + Mouse wheel scrolling | Zoom window                                              |
| Super + Left Alt              | Toggle input mode (movement / window input); see `input` |
| F3                            | Toggle debug HUD                                         |
| F5                            | Switch camera view                                       |

# <a name="configuration"></a> Configuration

## Lua example:

```lua
if hl.plugin.hypr3d then
    local hypr3d = hl.plugin.hypr3d

    hl.bind("SUPER + F12", hypr3d.toggle)
    
    hl.bind("SUPER + F8", function()
        if hypr3d.active() then
            -- only while the 3D view is open.
        end
    end)
    -- execute code when the 3D mode changes; open = true when 3D is enabled.
    hypr3d.on_state(function(open)
        -- example: in 2D border_size=5, in 3D border_size=0.
        hl.config({ general = { border_size = open and 0 or 5 } })
    end)

    -- config
    hypr3d.config({
        world = {
            panorama = "~/panorama.png",
            grid = false,
        },
        windows = {
            window_scale = 0.25,
            spawn_distance = 2,
        },
        input = {
            typing_cursor = true,
            typing_button = "side"
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

## Lua API

| Function                           | Description                                       |
| ------------------------------------| ---------------------------------------------------|
| `hypr3d.toggle()`                  | open the room if closed, close it if open         |
| `hypr3d.open()` / `hypr3d.close()` | explicit open / close                             |
| `hypr3d.active()`                  | `true` while the 3D room is open                  |
| `hypr3d.on_state(fn)`              | call `fn(open)` whenever the room opens or closes |
| `hypr3d.config({params})`          | see [Config parameters](#config-parameters)       |

## <a name="config-parameters"></a> Config parameters

Everything is optional -- only set what you want to change. 

### World

| Option       | Type   | Default | Description                                                                                                                                                           |
| --------------| --------| ---------| -----------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| panorama     | string | ""      | 360° background image (equirectangular)                                                                                                                               |
| grid         | bool   | true    | the starting 40x40 platform                                                                                                                                           |
| monitor      | string | ""      | render the 3D view on this monitor (e.g. "DP-1") instead of the focused one                                                                                           |
| under_layers | bool   | true    | draw the room under the top and overlay layers: bars, launchers, sidebars and notifications (e.g. Quickshell) stay 2D on top. only bottom-layer widgets join the room |

### Windows

| Option         | Type               | Default    | Description                              |
| ----------------| --------------------| ------------| ------------------------------------------|
| window_scale   | float              | 0.5        | window size multiplier (at 100 px/m)     |
| spawn_distance | float              | 5.0        | how far from you new windows appear (m)  |
| depth          | float              | 0.05       | window slab thickness (0 = flat quads)   |
| spawn_size     | [Vector2](#vector) | {960, 540} | size of windows spawned while 3D is open |

### Input

| Option        | Type          | Default | Description                                                                                                |
| ---------------| ---------------| ---------| ------------------------------------------------------------------------------------------------------------|
| typing_cursor | bool          | true    | input mode also frees the pointer                                                                          |
| typing_button | string/number | none    | a mouse button that toggles input mode: `side`, `extra`, `forward`, `back`, `task`, or a Linux button code |

### Player

| Option           | Type                                | Default     | Description                                           |
| ------------------| -------------------------------------| -------------| -------------------------------------------------------|
| mesh             | [Mesh](#mesh)                       | {}          | object's visual                                       |
| animations       | [Animation Group](#animation-group) | {}          | which clip each movement state plays                  |
| look_sensitivity | float                               | 0.0025      | how fast the camera turns (radians per pointer count) |
| look_inertia     | float                               | 0.03        | camera coasting after you stop the mouse (in seconds) |
| move_inertia     | float                               | 0.05        | coasting after you stop walking (in seconds)          |
| move_speed       | float                               | 4.0         | how fast you walk (m/s, running - 2.5x)               |
| spawn            | [Vector3](#vector)                  | { 0, 0, 0 } | player spawn point                                    |
| flying           | bool                                | true        | disables falling                                      |
| walk_bob         | bool                                | true        | simulate the rhythm of walking                        |
| collision        | bool                                | true        | whether body collides with the world                  |

### Scene

| Option          | Type                          | Default | Description                          |
| -----------------| -------------------------------| ---------| --------------------------------------|
| any unique name | [Scene Object](#scene-object) | {}      | you can place any number of objects. |

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
| center_offset  | [Vector3](#vector)      | {0, 0, 0} | Extra shift of the rotation pivot   |

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
| source         | int/string | none    | which anim to play: its name in the file, or its zero-based index |
| duration_scale | float      | 1.0     | playback speed multiplier                                         |

### <a name="transform"></a> Transform

| Option   | Type               | Default   |
| ----------| --------------------| -----------|
| position | [Vector3](#vector) | {0, 0, 0} |
| rotation | [Vector3](#vector) | {0, 0, 0} |
| scale    | [Vector3](#vector) | {1, 1, 1} |

### <a name="vector"></a> Vector

Vectors can be written either way: `{ x = 1, y = 2, z = 3 }` or just `{ 1, 2, 3 }`.

# Screenshots
<table>
  <tr>
    <td align="center" width="25%"><img src="images/Screenshot5.png" alt="screenshot"></td>
    <td align="center" width="25%"><img src="images/Screenshot4.png" alt="screenshot"></td>
  </tr>
  <tr>
    <td align="center" width="25%"><img src="images/Screenshot2.png" alt="screenshot"></td>
    <td align="center" width="25%"><img src="images/Screenshot3.png" alt="screenshot"></td>
  </tr>
<tr>
    <td align="center" width="25%"><img src="images/Screenshot6.png" alt="screenshot"></td>
    <td align="center" width="25%"><img src="images/Screenshot7.png" alt="screenshot"></td>
  </tr>
</table>
