_Last edited: 2026-04-26_

# CommittedManagedBuffer

See [managed_buffer.md](managed_buffer.md) for the full design and comparison with
`ReservedManagedBuffer`.

## Resource State on Creation

`initializeStorage` always creates the resource in `D3D12_RESOURCE_STATE_COMMON`, NOT in
`initialResourceState`. The `initialResourceState` field is only used for internal barrier
tracking. This is called out in a comment because it's surprising — if you set
`initialResourceState` to `UNORDERED_ACCESS`, the resource is still created as `COMMON`.
