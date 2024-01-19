# frozen_string_literal: true

autoload :A, File.join(__dir__, 'a.1_1_0')
A.new.yay

module B
  autoload :BAR, File.join(__dir__, 'a')
end
