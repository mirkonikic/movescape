// Decompiled by movescape from Aptos Move bytecode v10.
// Compiler policy: bytecode >= v10, Move language >= 2.2.
// Source-map names were used where available; other names are generated.
// 1 metadata entries are preserved in the decoded module.
module 0x42::Helper {

  #[persistent]
  public fun bump(value: u64): u64 {
    return value + 1u64
  }

}
