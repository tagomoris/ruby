module CMIVar
  def self.get_a
    NSTestCIVar.instance_variable_get(:@a)
  end
  def self.get_b
    NSTestMIVar.instance_variable_get(:b)
  end
  def self.get_c
    NSTestMIVar.instance_variable_get(:c)
  end
  def self.call_a
    NSTestCIVar.a
  end
  def self.call_b
    NSTestMIVar.b
  end
  def self.call_a_re
    NSTestCIVar.a_re
  end
  def self.call_b_re
    NSTestMIVar.b_re
  end
  def self.call_c_re
    NSTestMIVar.c_re
  end
end

reopen_cm NSTestCIVar
  def self.set_a(val)
    @a = val
  end
  def self.a_re
    @a
  end
end
reopen_cm NSTestMIVar
  def self.set_b(val)
    @b = val
  end
  def self.b_re
    @b
  end
  def self.c_re
    @c
  end
end

NSTestCIVar.set_a(2)
NSTestMIVar.set_b("y")
NSTestMIVar.instance_variable_set(:@c, :one)
