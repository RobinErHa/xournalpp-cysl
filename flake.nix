{
  description = "Dev shell: stable pkgs, old poppler";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-25.11";

    vuln-nixpkgs.url = "github:NixOS/nixpkgs/f76bef61369be38a10c7a1aa718782a60340d9ff"; # poppler-glib version 21.06.1

    render-helper = {
      url = "path:./src/util/pdf_render_helper";
    };
  };

  outputs =
    {
      self,
      nixpkgs,
      vuln-nixpkgs,
      render-helper,
    }:
    let
      system = "x86_64-linux";
      pkgs = nixpkgs.legacyPackages.${system}; # stable
      vulnPkgs = vuln-nixpkgs.legacyPackages.${system}; # pinned old
      helperPkg = render-helper.packages.${system}.default;
    in
    {
      devShells.${system}.default = pkgs.mkShell {
        buildInputs = with pkgs; [
          clang
          clang-tools

          gsettings-desktop-schemas
          adwaita-icon-theme
          alsa-lib
          cmake
          ninja
          gcc
          gnumake
          pkg-config
          libxml2
          libzip
          librsvg
          portaudio
          libsndfile
          qpdf
          lua5_4
          gtksourceview4
          help2man
          gtk3
          glib

          # firejail -> on NixOS have to enable the firejail system configuration `programs.firejail.enable = true`, such that firejail has access to directories

          vulnPkgs.poppler
          helperPkg
        ];

        #Export the gsetting-desktop-schemas for opening files in xournalpp
        shellHook = ''
          echo "Xournal++ dev shell (vulnerable-nixpkgs at ${vuln-nixpkgs.rev or "unknown"})"
          echo "Poppler version: $(pkg-config --modversion poppler-glib 2>/dev/null || echo "unknown")"

          export XDG_DATA_DIRS="$XDG_DATA_DIRS:${pkgs.gsettings-desktop-schemas}/share:${pkgs.gtk3}/share/gsettings-schemas/${pkgs.gtk3.name}"

          export XOPP_RENDER_HELPER="${helperPkg}/bin/xopp-render-helper"
          export XOPP_RENDER_FIREJAIL_PROFILE="${helperPkg}/share/xopp-render-helper/render_helper.profile"
          echo "Render helper: $XOPP_RENDER_HELPER"
          echo "Firejail Profile: $XOPP_RENDER_FIREJAIL_PROFILE"
        '';
      };

    };

}
