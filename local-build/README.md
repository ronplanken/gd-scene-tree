# Local macOS build without Xcode

The official template build needs the Xcode generator. This folder builds the
same sources with the Command Line Tools, Homebrew Qt and the OBS.app that is
installed in `/Applications`, then installs the bundle into the user plugin
folder. Use it for day-to-day development on a Mac; releases come from CI.

```sh
brew install cmake ninja qt
sh local-build/build.sh
```
