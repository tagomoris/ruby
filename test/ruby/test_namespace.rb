# frozen_string_literal: true

require 'test/unit'

class TestNamespace < Test::Unit::TestCase
  def setup
    Namespace.enabled = true
    @n = Namespace.new
  end

  def teardown
    Namespace.enabled = nil
  end

  def test_namespace_availability
    Namespace.enabled = nil
    assert !Namespace.enabled
    Namespace.enabled = true
    assert Namespace.enabled
    Namespace.enabled = false
    assert !Namespace.enabled
  end

  def test_current_namespace
    $TESTING_NAMESPACES = []
    @n.require_relative('namespace/current')
    assert_equal 1, $TESTING_NAMESPACES.size
    assert $TESTING_NAMESPACES[0].is_a?(Namespace)
    assert_equal @n, $TESTING_NAMESPACES[0]
  end

  def test_require_rb_separately
    assert_raise(NameError) { NS_A } # !
    assert_raise(NameError) { NS_B }

    @n.require(File.join(__dir__, 'namespace', 'a.1_1_0'))
    assert_not_nil @n::NS_A
    assert_not_nil @n::NS_B
    assert_equal "1.1.0", @n::NS_A::VERSION
    assert_equal "yay 1.1.0", @n::NS_A.new.yay
    assert_equal "1.1.0", @n::NS_B::VERSION
    assert_equal "yay_b1", @n::NS_B.yay

    assert_raise(NameError) { NS_A }
    assert_raise(NameError) { NS_B }
  end

  def test_require_relative_rb_separately
    assert_raise(NameError) { NS_A }
    assert_raise(NameError) { NS_B }

    @n.require_relative('namespace/a.1_1_0')
    assert_not_nil @n::NS_A
    assert_not_nil @n::NS_B
    assert_equal "1.1.0", @n::NS_A::VERSION
    assert_equal "yay 1.1.0", @n::NS_A.new.yay
    assert_equal "1.1.0", @n::NS_B::VERSION
    assert_equal "yay_b1", @n::NS_B.yay

    assert_raise(NameError) { NS_A }
    assert_raise(NameError) { NS_B }
  end

  def test_load_separately
    assert_raise(NameError) { NS_A } # !
    assert_raise(NameError) { NS_B }

    @n.load(File.join('namespace', 'a.1_1_0.rb'))
    assert_not_nil @n::NS_A
    assert_not_nil @n::NS_B
    assert_equal "1.1.0", @n::NS_A::VERSION
    assert_equal "yay 1.1.0", @n::NS_A.new.yay
    assert_equal "1.1.0", @n::NS_B::VERSION
    assert_equal "yay_b1", @n::NS_B.yay

    assert_raise(NameError) { NS_A }
    assert_raise(NameError) { NS_B }
  end

  def test_namespace_in_namespace
    assert_raise(NameError) { NS1 }
    assert_raise(NameError) { NS_A } # !
    assert_raise(NameError) { NS_B }

    @n.require_relative('namespace/ns')
    assert_not_nil @n::NS1
    assert_not_nil @n::NS1::NS_A
    assert_not_nil @n::NS1::NS_B
    assert_equal "1.1.0", @n::NS1::NS_A::VERSION
    assert_equal "yay 1.1.0", @n::NS1::NS_A.new.yay
    assert_equal "1.1.0", @n::NS1::NS_B::VERSION
    assert_equal "yay_b1", @n::NS1::NS_B.yay

    assert_raise(NameError) { NS1 }
    assert_raise(NameError) { NS_A }
    assert_raise(NameError) { NS_B }
  end

  def test_require_rb_2versions
    assert_raise(NameError) { NS_A } # !

    @n.require(File.join(__dir__, 'namespace', 'a.1_2_0'))
    assert_equal "1.2.0", @n::NS_A::VERSION
    assert_equal "yay 1.2.0", @n::NS_A.new.yay

    n2 = Namespace.new
    n2.require(File.join(__dir__, 'namespace', 'a.1_1_0'))
    assert_equal "1.1.0", n2::NS_A::VERSION
    assert_equal "yay 1.1.0", n2::NS_A.new.yay

    # recheck @n is not affected by the following require
    assert_equal "1.2.0", @n::NS_A::VERSION
    assert_equal "yay 1.2.0", @n::NS_A.new.yay

    assert_raise(NameError) { NS_A }
  end

  def test_raising_errors_in_require
    assert_raise(RuntimeError, "Yay!") { @n.require(File.join(__dir__, 'namespace', 'raise')) }
    assert_nil Namespace.current
  end

  def test_autoload_in_namespace
    assert_raise(NameError) { NS_A }

    @n.require_relative('namespace/autoloading')
    # autoloaded A is visible from global
    assert_equal '1.1.0', @n::NS_A::VERSION

    assert_raise(NameError) { NS_A }

    # autoload trigger B::BAR is valid even from global
    assert_equal 'bar_b1', @n::NS_B::BAR # Oops, the autoload was triggered in the global namespace

    assert_raise(NameError) { NS_A }
  end

  def test_continuous_top_level_method_in_a_namespace
    @n.require_relative('namespace/define_toplevel')
    @n.require_relative('namespace/call_toplevel')
    assert_raise(NameError) { foo }
  end

  def test_top_level_methods_in_namespace
    # TODO: top-level per namespace, can be referred from any Objects
    pend
    @n.require_relative('namespace/top_level')
    assert_equal "yay!", @n::Foo.foo
    assert_raise(NameError) { yaaay }
  end
end
