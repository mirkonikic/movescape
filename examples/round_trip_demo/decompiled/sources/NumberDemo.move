// Decompiled by movescape from Aptos Move bytecode v10.
// Compiler policy: bytecode >= v10, Move language >= 2.2.
// Source-map names were used where available; other names are generated.
// 1 metadata entries are preserved in the decoded module.
module 0x42::NumberDemo {

  #[persistent]
  public fun checked(value: u8, owner: address): u8 {
    assert!(owner != @0x0, 77u64);
    return value
  }

  #[persistent]
  public fun measure(value: u8, owner: address): u8 {
    let _t4: bool;

    _t4 = owner == @0x42 && value < 255u8;
    return if (_t4) { value + 1u8 } else { value }
  }

}
