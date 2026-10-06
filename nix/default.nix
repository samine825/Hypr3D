{
  lib,
  pkgs,
  hyprland,
  version ? "0.5.0",
  system ? builtins.currentSystem,
}: let
  hyprlandPackage = hyprland.packages.${system}.hyprland;

  # Hyprland 0.56.2's CMake requires glaze >= 7 < 8, while the nixpkgs it
  # pins already carries glaze 8 -- its own build falls back to a network
  # FetchContent clone, which a Nix build can never do. Pin the source and
  # hand it to FetchContent directly instead.
  glaze72 = pkgs.fetchFromGitHub {
    owner = "stephenberry";
    repo = "glaze";
    rev = "v7.2.0";
    hash = "sha256-f3NVRi3SXKo42hn0WCw7JsOK3EkdOVJIcuzhPorKjFY=";
  };

  hyprlandFixed = hyprlandPackage.overrideAttrs (old: {
    cmakeFlags = (old.cmakeFlags or [])
      ++ ["-DFETCHCONTENT_SOURCE_DIR_GLAZE=${glaze72}"];
    nativeBuildInputs = (old.nativeBuildInputs or []) ++ [pkgs.git];
  });
in
  hyprlandFixed.stdenv.mkDerivation {
    pname = "hypr3d";
    inherit version;

    src = ../.;

    nativeBuildInputs = with pkgs; [cmake pkg-config];

    # hyprland's own build inputs carry the shared hyprwm libraries and the
    # graphics stack the ABI was built against; the explicit list is what
    # this plugin's CMakeLists pkg_check_modules look up directly.
    buildInputs =
      with pkgs;
        [hyprgraphics pixman libdrm libxkbcommon libGL]
        ++ hyprlandFixed.buildInputs;

    cmakeFlags = [
      "-DHYPRLAND_HEADERS=${hyprlandFixed.dev}"
      "-DCMAKE_BUILD_TYPE=Release"
    ];

    # The CMake project produces the plugin .so but has no install rules:
    # pick the single shared object out of the build tree and give it a
    # stable name so users can write
    #   plugin = ${pkgs.hypr3d}/lib/hypr3d.so
    installPhase = ''
      runHook preInstall

      local SO
      SO="$(find . -type f -name '*.so' ! -path '*/CMakeFiles/*' | head -n1)"
      test -n "$SO" || {
        echo "no plugin .so produced by the cmake build" >&2
        exit 1
      }
      mkdir -p "$out/lib"
      install -Dm755 "$SO" "$out/lib/hypr3d.so"

      runHook postInstall
    '';

    meta = with lib; {
      homepage = "https://github.com/samine825/Hypr3D";
      description = "A walkable 3D room workspace plugin for Hyprland";
      license = licenses.mit;
      platforms = platforms.linux;
    };
  }
