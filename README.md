# Flow-IPC Sub-project -- Structured Transport -- Transport of Cap'n Proto-backed structured messages

This project is a sub-project of the larger Flow-IPC meta-project.  Please see
a similar `README.md` for Flow-IPC, first.  You can most likely find it either in the parent
directory to this one; or else in a sibling GitHub repository named `ipc.git`.

A more grounded description of the various sub-projects of Flow-IPC, including this one, can be found
in `./src/ipc/common.hpp` off the directory containing the present README.  Look for
`Distributed sub-components (libraries)` in a large C++ comment.

Took a look at those?  Still interested in `ipc_transport_structured` as an independent entity?  Then read on.
Before you do though: it is, typically, both easier and more functional to simply treat Flow-IPC as a whole.
To do so it is sufficient to never have to delve into topics discussed in this README.  In particular
the Flow-IPC generated documentation guided Manual + Reference are monolithic and cover all the
sub-projects together, including this one.

Still interested?  Then read on.

`ipc_transport_structured` depends on `ipc_core` (and all its dependencies; i.e. `flow`).  It provides
`ipc::transport::struc` (excluding `ipc::transport::struc::shm`).  The star of the show is
`ipc::transport::struc::Channel` through which one can send/receive/expect/etc. Cap'n Proto-backed
messages.  That said, with `ipc_transport_structured` but without `ipc_shm` the transport of these
messages is not quite as fast as it can be: while capnp's (Cap'n Proto) *zero-copy* feature is used
to its fullest, one must still copy a serialization into a transport (e.g., Unix domain socket stream
or POSIX MQ) and on the other copy it out of there.  However, with `ipc_shm` (which depends on us)
one gets **full end-to-end zero-copy performance**, as the serialization is *never* copied.

Thus, `ipc_transport_structured` provides the **serializer and deserializer concepts** and the
**heap serializer** and **heap deserializer** implementations of those concepts.  `ipc_shm`
further adds **SHM serializer** and **SHM deserializer** implementations of those concepts.

Nevertheless, for some applications, it makes sense to depend on the simpler heap serializer provided here.
Your application code would be almost identical either way.

## Documentation

See Flow-IPC meta-project's `README.md` Documentation section.  `ipc_transport_structured` lacks its own generated docs.
However, it contributes to the aforementioned monolithic documentation through its many comments which can
(of course) be found directly in its code (`./src/ipc/...`).  (The monolithic generated documentation scans
these comments using Doxygen, combined with its siblings' comments... and so on.)

## Obtaining the source code

- As a tarball/zip: The [project web site](https://flow-ipc.github.io) links to individual releases with notes, docs,
  download links.  We are included in a subdirectory off the Flow-IPC root.
- For Git access:
  - `git clone --recurse-submodules git@github.com:Flow-IPC/ipc.git`; or
  - `git clone git@github.com:Flow-IPC/ipc_transport_structured.git`

## Installation

See [INSTALL](./INSTALL.md) guide.

## Contributing

See [CONTRIBUTING](./CONTRIBUTING.md) guide.
