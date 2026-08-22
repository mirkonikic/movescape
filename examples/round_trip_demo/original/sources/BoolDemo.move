module 0x42::BoolDemo {
  public fun xor(left: bool, right: bool): bool {
    (left || right) && !(left && right)
  }
}
