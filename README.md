# Photon Engine

Photon Engine is [Photon](https://github.com/PhotonBrowser/photon)'s web engine, maintained as a downstream of [Ladybird](https://github.com/LadybirdBrowser/ladybird). It provides web rendering, browser services, and the C++ embedder API used by Photon's Rust/GPUI-CE shell.

## Development

For the easiest setup, build Photon from the [main browser repository](https://github.com/PhotonBrowser/photon). It pins Photon Engine as the `Engine/` submodule and provides the development CLI.

- [Contributor setup](https://github.com/PhotonBrowser/photon/blob/main/CONTRIBUTOR_SETUP.md)
- [Building and running Photon](https://github.com/PhotonBrowser/photon/blob/main/Documentation/Building.md)
- [Architecture](https://github.com/PhotonBrowser/photon/blob/main/Documentation/Architecture.md)
- [Upstream maintenance and submodules](https://github.com/PhotonBrowser/photon/blob/main/Documentation/Upstream.md)
- [Contributing](https://github.com/PhotonBrowser/photon/blob/main/CONTRIBUTING.md)

Ladybird uses a [multi-process architecture](Documentation/ProcessArchitecture.md) with a main UI process,
WebContent renderer processes, and helper processes for tasks such as networking, image decoding, compositing and media.

Documents from different sites run in separate renderer processes, each sandboxed from the rest of the system.
Image decoding and network connections are done out of process, separately for each site, to be more robust against
malicious content.

Photon Engine regularly incorporates changes from Ladybird. Keep downstream changes focused and compatible with upstream wherever possible. Follow the [upstream maintenance guide](https://github.com/PhotonBrowser/photon/blob/main/Documentation/Upstream.md) when syncing or updating the pinned engine revision.

Photon Engine retains Ladybird's copyright notices and [BSD 2-Clause license](LICENSE). See [engine contribution guidance](CONTRIBUTING.md) for repository-specific policies.
