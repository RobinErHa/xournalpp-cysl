{
  description = "Dev shell: stable pkgs, old poppler";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-25.11";

    vuln-nixpkgs.url = "github:NixOS/nixpkgs/f76bef61369be38a10c7a1aa718782a60340d9ff"; # poppler-glib version 21.06.1
  };

  outputs =
    {
      self,
      nixpkgs,
      vuln-nixpkgs,
    }:
    let
      system = "x86_64-linux";
      pkgs = nixpkgs.legacyPackages.${system}; # stable
      vulnPkgs = vuln-nixpkgs.legacyPackages.${system}; # pinned old
    in
    {
      devShells.${system}.default = pkgs.mkShell {
        buildInputs = with pkgs; [
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

          vulnPkgs.poppler
        ];

        #Export the gsetting-desktop-schemas for opening files in xournalpp
        shellHook = ''
          echo "Xournal++ dev shell (vulnerable-nixpkgs at ${vuln-nixpkgs.rev or "unknown"})"
          echo "Poppler version: $(pkg-config --modversion poppler-glib 2>/dev/null || echo "unknown")"

          export XDG_DATA_DIRS="$XDG_DATA_DIRS:${pkgs.gsettings-desktop-schemas}/share:${pkgs.gtk3}/share/gsettings-schemas/${pkgs.gtk3.name}"
        '';
      };

    };

}
