def yaaay
  "yay!"
end

module Foo
  def self.foo
    yaaay
  end
end

Foo.foo # Should not raise NameError
