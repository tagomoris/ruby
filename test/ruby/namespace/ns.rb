# frozen_string_literal: true

NS1 = Namespace.new
NS1.require_relative('a.1_1_0')

def yay
  NS1::B::yay
end

yay
