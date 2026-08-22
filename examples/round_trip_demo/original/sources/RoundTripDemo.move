module 0x42::RoundTripDemo {
  use 0x42::Helper;

  enum Choice has drop {
    First { value: u64 },
    Second { value: u64 },
    Empty,
  }

  public fun first(value: u64): Choice {
    Choice::First { value }
  }

  public fun second(value: u64): Choice {
    Choice::Second { value }
  }

  public fun empty(): Choice {
    Choice::Empty
  }

  public fun is_number(choice: &Choice): bool {
    choice is First|Second
  }

  public fun compute(flag: bool, value: u64): u64 {
    assert!(value < 1000, 7);
    let adjusted: u64;
    if (flag) adjusted = value + 10 else adjusted = value + 20;

    let remaining = 3;
    loop {
      adjusted = Helper::bump(adjusted);
      remaining = remaining - 1;
      if (remaining == 0) break;
    };
    adjusted
  }

  public fun add(x: u64, y: u64): u64 {
    x + y
  }

  public fun identity<Element>(value: Element): Element {
    value
  }

  public fun delayed(x: u64): |u64|u64 has copy+drop+store {
    |y| add(x, y)
  }
}
