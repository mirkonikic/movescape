module 0x42::NumberDemo {
  public fun measure(value: u8, owner: address): u8 {
    if (owner == @0x42 && value < 255) {
      value + 1
    } else {
      value
    }
  }

  public fun checked(value: u8, owner: address): u8 {
    assert!(owner != @0x0, 77);
    value
  }
}
