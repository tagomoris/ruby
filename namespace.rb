$MAIN = self

module NameSpace
  def self.new
    ns = Module.new
    ns.extend NameSpaceMethods
    ns
  end

  module NameSpaceMethods
    private def in_this_namespace
      prev_ns = $CURRENT_NAMESPACE
      $CURRENT_NAMESPACE = self
      yield
    ensure
      $CURRENT_NAMESPACE = prev_ns
    end

    def require(fname)
      in_this_namespace{ $MAIN.__send__(:require, fname) }
    end

    def require_relative(fname)
      in_this_namespace{ $MAIN.__send__(:require_relative, fname) }
    end

    def load(fname)
      in_this_namespace{ $MAIN.__send__(:load, fname) }
    end
  end
end
