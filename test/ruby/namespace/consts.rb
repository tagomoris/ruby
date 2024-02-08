reopen_cm String
  STR_CONST1 = 111
  STR_CONST2 = 222
  STR_CONST3 = 333
end

reopen_cm String
  STR_CONST1 = 112

  def refer1
    STR_CONST1
  end

  def refer2
    STR_CONST2
  end
end

module ForConsts
  CONST1 = 111
end

TOP_CONST = 10

module ForConsts
  CONST1 = 112
  CONST2 = 222
  CONST3 = 333

  def self.refer_all
    ForConsts::CONST1
    ForConsts::CONST2
    ForConsts::CONST3
    String::STR_CONST1
    String::STR_CONST2
    String::STR_CONST3
  end

  def self.refer1
    CONST1
  end

  def self.refer2
    CONST2
  end

  def self.refer3
    CONST3
  end

  def self.refer_top_const
    TOP_CONST
  end

  # for String
  class Proxy
    def call_str_refer1
      String.new.refer1
    end

    String::STR_CONST2 = 223

    def call_str_refer2
      String.new.refer2
    end

    # for Integer
    Integer::INT_CONST1 = 1

    def refer_int_const1
      Integer::INT_CONST1
    end
  end
end

# should not raise errors
ForConsts.refer_all
String::STR_CONST1
Integer::INT_CONST1

# TODO: If we execute this sentence once, the constant value will be cached on ISeq inline constant cache.
#       And it changes the behavior of ForConsts.refer_consts_directly called from global.
# ForConsts.refer_consts_directly # should not raise errors too
