{
  description = "Hypr3D: a walkable 3D room workspace plugin for Hyprland";

  inputs = {
    # The plugin's ABI matches the Hyprland version it is built against. If
    # your system's Hyprland comes from a different source, point this input
    # at it, e.g. in your flake:
    #   inputs.hypr3d.url = "github:samine825/Hypr3D";
    #   inputs.hypr3d.inputs.hyprland.follows = "hyprland";
    hyprland.url = "git+https://github.com/hyprwm/Hyprland?rev=efb50993780079460b0cbed1363e2166a2de1d9f";
    nixpkgs.follows = "hyprland/nixpkgs";
    systems.follows = "hyprland/systems";
  };

  outputs = {
    self,
    hyprland,
    nixpkgs,
    systems,
    ...
  }: let
    inherit (nixpkgs.lib) genAttrs;
    eachSystem = genAttrs (import systems);
  in {
    packages = eachSystem (system: rec {
      hypr3d = nixpkgs.legacyPackages.${system}.callPackage ./nix/default.nix {
        inherit hyprland system;
        pkgs = nixpkgs.legacyPackages.${system};
        version = "0.5.0" + "+" + (self.shortRev or self.dirtyShortRev or "unknown");
      };
      default = hypr3d;
    });

    checks = eachSystem (system: self.packages.${system});

    devShells = eachSystem (system:
      let pkgs = nixpkgs.legacyPackages.${system}; in {
        default = pkgs.mkShell {
          name = "hypr3d";

          nativeBuildInputs = with pkgs; [cmake pkg-config];
          inputsFrom = [self.packages.${system}.hypr3d];

          shellHook = ''
            export HYPRLAND_HEADERS=${hyprland.packages.${system}.hyprland.dev}
          '';
        };
      });
  };
}
