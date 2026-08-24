# Publishing checklist

The repository is prepared to be published without generated dependencies, build products, disc images, or extracted game assets.

## First publish

```bash
git init
git add .
git commit -m "Initial native Linux Aurora port"
git branch -M main
git remote add origin <repository-url>
git push -u origin main
```

After the push, GitHub Actions runs the native Linux build from `.github/workflows/native-linux.yml`.

## Before a release

```bash
./build.sh --clean --portable
./run.sh /path/to/Pikmin.iso
```

Verify at least:

- boot/title flow
- gameplay entry
- controller input
- save/load
- fullscreen + Alt+Tab
- STX playback
- native JAudio music/SE path

Do not commit disc images, extracted runtime cache contents, build directories, or bootstrapped third-party checkouts.
