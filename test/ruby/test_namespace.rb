# frozen_string_literal: true

require 'test/unit'

class TestNamespace < Test::Unit::TestCase
  def setup
    @n = Namespace.new
  end

  def test_current_namespace
    $TESTING_NAMESPACES = []
    @n.require_relative('namespace/current')
    assert_equal 1, $TESTING_NAMESPACES.size
    assert $TESTING_NAMESPACES[0].is_a?(Namespace)
    assert_equal @n, $TESTING_NAMESPACES[0]
  end

  def test_require_rb_separately
    assert_raise(NameError) { A }
    assert_raise(NameError) { B }

    @n.require(File.join(__dir__, 'namespace', 'a.1_1_0'))
    assert_not_nil @n::A
    assert_not_nil @n::B
    assert_equal "1.1.0", @n::A::VERSION
    assert_equal "yay 1.1.0", @n::A.new.yay
    assert_equal "1.1.0", @n::B::VERSION
    assert_equal "yay_b1", @n::B.yay

    assert_raise(NameError) { A }
    assert_raise(NameError) { B }
  end

  def test_require_relative_rb_separately
    assert_raise(NameError) { A }
    assert_raise(NameError) { B }

    @n.require_relative('namespace/a.1_1_0')
    assert_not_nil @n::A
    assert_not_nil @n::B
    assert_equal "1.1.0", @n::A::VERSION
    assert_equal "yay 1.1.0", @n::A.new.yay
    assert_equal "1.1.0", @n::B::VERSION
    assert_equal "yay_b1", @n::B.yay

    assert_raise(NameError) { A }
    assert_raise(NameError) { B }
  end

  def test_load_separately
    assert_raise(NameError) { A }
    assert_raise(NameError) { B }

    @n.load(File.join('namespace', 'a.1_1_0.rb'))
    assert_not_nil @n::A
    assert_not_nil @n::B
    assert_equal "1.1.0", @n::A::VERSION
    assert_equal "yay 1.1.0", @n::A.new.yay
    assert_equal "1.1.0", @n::B::VERSION
    assert_equal "yay_b1", @n::B.yay

    assert_raise(NameError) { A }
    assert_raise(NameError) { B }
  end

  def test_namespace_in_namespace
    assert_raise(NameError) { NS1 }
    assert_raise(NameError) { A }
    assert_raise(NameError) { B }

    @n.require_relative('namespace/ns')
    assert_not_nil @n::NS1
    assert_not_nil @n::NS1::A
    assert_not_nil @n::NS1::B
    assert_equal "1.1.0", @n::NS1::A::VERSION
    assert_equal "yay 1.1.0", @n::NS1::A.new.yay
    assert_equal "1.1.0", @n::NS1::B::VERSION
    assert_equal "yay_b1", @n::NS1::B.yay

    assert_raise(NameError) { NS1 }
    assert_raise(NameError) { A }
    assert_raise(NameError) { B }
  end

  def test_require_rb_2versions
    @n.require(File.join(__dir__, 'namespace', 'a.1_2_0'))
    assert_equal "1.2.0", @n::A::VERSION
    assert_equal "yay 1.2.0", @n::A.new.yay

    n2 = Namespace.new
    n2.require(File.join(__dir__, 'namespace', 'a.1_1_0'))
    assert_equal "1.1.0", n2::A::VERSION
    assert_equal "yay 1.1.0", n2::A.new.yay

    # recheck @n is not affected by the following require
    assert_equal "1.2.0", @n::A::VERSION
    assert_equal "yay 1.2.0", @n::A.new.yay
  end

  def test_raising_errors_in_require
    assert_raise(RuntimeError, "Yay!") { @n.require(File.join(__dir__, 'namespace', 'raise')) }
    assert_nil Namespace.current
  end
end
