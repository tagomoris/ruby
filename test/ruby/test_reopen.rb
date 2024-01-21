# frozen_string_literal: true
require 'test/unit'

class TestReopen < Test::Unit::TestCase
  def test_reopen_missing_constant
    assert_separately([], "#{<<~"begin;"}\n#{<<~'end;'}")
    begin;
      assert_raise_with_message(ArgumentError, /MissingName is not defined yet/) do
        reopen_cm MissingName
          def adding_method
            "yay"
          end
        end
      end
    end;
  end

  def test_reopen_existing_class
    assert_separately([], "#{<<~"begin;"}\n#{<<~'end;'}")
    begin;
      assert_raise_with_message(RuntimeError, /yay/) do
        reopen_cm String
          def adding_method
            "yay"
          end
        end
        raise "yaaay".adding_method
      end
    end;
  end

  def test_reopen_existing_module
    assert_separately([], "#{<<~"begin;"}\n#{<<~'end;'}")
    begin;
      assert_raise_with_message(RuntimeError, /yay/) do
        module Foo
        end
        reopen_cm Foo
          def self.adding_method
            "yay"
          end
        end
        raise Foo.adding_method
      end
    end;
  end
end
