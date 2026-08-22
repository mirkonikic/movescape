#[test_only]
module 0x42::BehaviorTests {
  use 0x42::BoolDemo;
  use 0x42::NumberDemo;
  use 0x42::RoundTripDemo;
  use 0x42::ResourceDemo;

  #[test]
  fun compute_both_branches_and_loop() {
    assert!(RoundTripDemo::compute(true, 5) == 18, 100);
    assert!(RoundTripDemo::compute(false, 5) == 28, 101);
  }

  #[test]
  #[expected_failure(abort_code = 7, location = 0x42::RoundTripDemo)]
  fun compute_preserves_abort() {
    RoundTripDemo::compute(true, 1000);
  }

  #[test]
  fun enum_variant_list() {
    let first = RoundTripDemo::first(9);
    let second = RoundTripDemo::second(10);
    let empty = RoundTripDemo::empty();
    assert!(RoundTripDemo::is_number(&first), 102);
    assert!(RoundTripDemo::is_number(&second), 103);
    assert!(!RoundTripDemo::is_number(&empty), 104);
  }

  #[test]
  fun closure_and_generic_identity() {
    let add_five = RoundTripDemo::delayed(5);
    assert!(add_five(7) == 12, 105);
    assert!(RoundTripDemo::identity<u64>(77) == 77, 106);
  }

  #[test(account = @0xcafe)]
  fun resource_lifecycle(account: signer) {
    assert!(!ResourceDemo::contains(@0xcafe), 107);
    ResourceDemo::publish(&account, 41);
    assert!(ResourceDemo::contains(@0xcafe), 108);
    assert!(ResourceDemo::value(@0xcafe) == 41, 109);
    ResourceDemo::increment(@0xcafe);
    assert!(ResourceDemo::value(@0xcafe) == 42, 110);
    assert!(ResourceDemo::remove(@0xcafe) == 42, 111);
    assert!(!ResourceDemo::contains(@0xcafe), 112);
  }

  #[test(alice = @0xa11ce, bob = @0xb0b)]
  fun resources_are_account_isolated(alice: signer, bob: signer) {
    ResourceDemo::publish(&alice, 5);
    ResourceDemo::publish(&bob, 9);
    ResourceDemo::increment(@0xa11ce);
    assert!(ResourceDemo::value(@0xa11ce) == 6, 113);
    assert!(ResourceDemo::value(@0xb0b) == 9, 114);
    assert!(ResourceDemo::remove(@0xa11ce) == 6, 115);
    assert!(ResourceDemo::remove(@0xb0b) == 9, 116);
  }

  #[test]
  fun boolean_truth_table() {
    assert!(!BoolDemo::xor(false, false), 117);
    assert!(BoolDemo::xor(true, false), 118);
    assert!(BoolDemo::xor(false, true), 119);
    assert!(!BoolDemo::xor(true, true), 120);
  }

  #[test]
  fun numeric_and_address_inputs() {
    assert!(NumberDemo::measure(0, @0x0) == 0, 121);
    assert!(NumberDemo::measure(0, @0x42) == 1, 122);
    assert!(NumberDemo::measure(254, @0x42) == 255, 123);
    assert!(NumberDemo::measure(255, @0x42) == 255, 124);
  }

  #[test]
  #[expected_failure(abort_code = 77, location = 0x42::NumberDemo)]
  fun generated_domain_abort_origin() {
    NumberDemo::checked(7, @0x0);
  }
}
