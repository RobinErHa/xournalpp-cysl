{
  description = "PDF render helper with vulnerable poppler version";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/f76bef61369be38a10c7a1aa718782a60340d9ff"; # poppler-glib version 21.06.1
  };

  outputs =
    { self, nixpkgs }:
    let
      system = "x86_64-linux";
      pkgs = nixpkgs.legacyPackages.${system};
    in
    {
      packages.${system}.default = pkgs.stdenv.mkDerivation {
        pname = "xopp-render-helper";
        version = "0.1";

        src = ./.;

        nativeBuildInputs = [ pkgs.pkg-config ];
        buildInputs = [
          pkgs.poppler
          pkgs.cairo
          pkgs.glib
        ];

        dontConfigure = true;

        buildPhase = ''
          runHook preBuild
          $CXX -std=c++20 -O2 -Wall -Wextra \
              $(pkg-config --cflags poppler-glib cairo glib-2.0 gobject-2.0) \
              main.cpp \
              $(pkg-config --libs   poppler-glib cairo glib-2.0 gobject-2.0) \
              -o xopp-render-helper
          runHook postBuild
        '';

        installPhase = ''
          runHook preInstall
          mkdir -p $out/bin
          install -m755 xopp-render-helper $out/bin/

          mkdir -p $out/share/xopp-render-helper
          install -m644 render_helper.profile $out/share/xopp-render-helper/

          runHook postInstall
        '';

        meta = with pkgs.lib; {
          description = "Sandboxed PDF render helper for Xournal++";
          platforms = platforms.linux;
          mainProgram = "xopp-render-helper";
        };
      };

    };
}
