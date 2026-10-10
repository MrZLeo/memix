{
  description = "C++ development environment for memix";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixpkgs-unstable";

  outputs =
    { nixpkgs, ... }:
    let
      forAllSystems = nixpkgs.lib.genAttrs [
        "x86_64-linux"
        "aarch64-linux"
        "aarch64-darwin"
      ];
    in
    {
      devShells = forAllSystems (
        system:
        let
          pkgs = nixpkgs.legacyPackages.${system};
          stdenv =
            if pkgs.stdenv.hostPlatform.isDarwin then
              pkgs.llvmPackages_latest.stdenv
            else
              pkgs.overrideCC pkgs.gccStdenv pkgs.gcc_latest;
        in
        {
          default = (pkgs.mkShell.override { inherit stdenv; }) {
            packages = with pkgs; [
              cmake
              ninja
              git
              pkg-config
              ccache
              llvmPackages_latest.clang-tools
            ];

            # Keep static libc available
            env = pkgs.lib.optionalAttrs pkgs.stdenv.hostPlatform.isLinux {
              LIBRARY_PATH = "${pkgs.glibc.static}/lib";
            };
            CMAKE_GENERATOR = "Ninja";
          };
        }
      );

      formatter = forAllSystems (system: nixpkgs.legacyPackages.${system}.nixfmt);
    };
}
