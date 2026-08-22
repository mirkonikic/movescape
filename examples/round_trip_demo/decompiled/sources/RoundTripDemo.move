// Decompiled by movescape from Aptos Move bytecode v10.
// Compiler policy: bytecode >= v10, Move language >= 2.2.
// Source-map names were used where available; other names are generated.
// 1 metadata entries are preserved in the decoded module.
module 0x42::RoundTripDemo {

  use 0x42::Helper;

  enum Choice has drop {
    First {
      value: u64,
    },
    Second {
      value: u64,
    },
    Empty,
  }

  #[persistent]
  public fun add(x: u64, y: u64): u64 {
    return x + y
  }

  #[persistent]
  public fun compute(flag: bool, value: u64): u64 {
    let _t4: u64;
    let _t5: u64;

    assert!(value < 1000u64, 7u64);
    _t4 = if (flag) { value + 10u64 } else { value + 20u64 };
    _t5 = 3u64;
    loop {
      _t4 = Helper::bump(_t4);
      _t5 = _t5 - 1u64;
      if (_t5 == 0u64) {
        break;
      } else {
        continue;
      }
    }
    return _t4
  }

  #[persistent]
  public fun delayed(x: u64): |u64|(u64) has copy + drop + store {
    return (|closure_arg1| add(x, closure_arg1))
  }

  #[persistent]
  public fun empty(): Choice {
    return Choice::Empty {  }
  }

  #[persistent]
  public fun first(value: u64): Choice {
    return Choice::First { value: value }
  }

  #[persistent]
  public fun identity<Element>(value: Element): Element {
    return move value
  }

  #[persistent]
  public fun is_number(choice: &Choice): bool {
    return (copy choice is First|Second)
  }

  #[persistent]
  public fun second(value: u64): Choice {
    return Choice::Second { value: value }
  }

}
