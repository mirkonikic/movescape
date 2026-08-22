// Decompiled by movescape from Aptos Move bytecode v10.
// Compiler policy: bytecode >= v10, Move language >= 2.2.
// Source-map names were used where available; other names are generated.
// 1 metadata entries are preserved in the decoded module.
module 0x42::BoolDemo {

  #[persistent]
  public fun xor(left: bool, right: bool): bool {
    let _t3: bool;
    let _t4: bool;

    _t3 = left || right;
    if (_t3) {
      _t4 = left && right;
      return !_t4
    } else {
      return false
    }
  }

}
