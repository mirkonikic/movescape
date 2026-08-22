// Decompiled by movescape from Aptos Move bytecode v10.
// Compiler policy: bytecode >= v10, Move language >= 2.2.
// Source-map names were used where available; other names are generated.
// 1 metadata entries are preserved in the decoded module.
module 0x42::ResourceDemo {

  struct Counter has key {
    value: u64,
  }

  #[persistent]
  public fun contains(owner: address): bool {
    return exists<Counter>(owner)
  }

  #[persistent]
  public fun publish(account: &signer, initial: u64) {
    move_to<Counter>(move account, Counter { value: initial });
    return;
  }

  #[persistent]
  public fun remove(owner: address): u64 acquires Counter {
    let Counter { value: tmp2 } = move_from<Counter>(owner);
    return tmp2
  }

  #[persistent]
  public fun value(owner: address): u64 acquires Counter {
    return borrow_global<Counter>(owner).value
  }

  #[persistent]
  public fun increment(owner: address) acquires Counter {
    let counter: &mut Counter;

    counter = borrow_global_mut<Counter>(owner);
    counter.value = counter.value + 1u64;
    return;
  }

}
