module 0x42::ResourceDemo {
  struct Counter has key {
    value: u64,
  }

  public fun publish(account: &signer, initial: u64) {
    move_to(account, Counter { value: initial });
  }

  public fun contains(owner: address): bool {
    exists<Counter>(owner)
  }

  public fun value(owner: address): u64 acquires Counter {
    let counter = borrow_global<Counter>(owner);
    counter.value
  }

  public fun increment(owner: address) acquires Counter {
    let counter = borrow_global_mut<Counter>(owner);
    counter.value = counter.value + 1;
  }

  public fun remove(owner: address): u64 acquires Counter {
    let Counter { value } = move_from<Counter>(owner);
    value
  }
}
