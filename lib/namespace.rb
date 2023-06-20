$MAIN = self

module NameSpace
  def self.define
    ns = Module.new
    ns.extend NameSpaceMethods
    ns.instance_variable_set(:@features, [])
    ns.instance_variable_set(:@realpaths, {}) # realpath => Qtrue
    ns.instance_variable_set(:@realpath_map, {}) # feature => realpath
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

    def is_loaded_feature?(fname, rb, ext, expanded)
      if expanded
        if @features.include?(fname)
          return fname.end_with?('.rb') ? '.rb' : '.so' # non-'.rb' extension is not always '.so', but anyway, it's not '.rb'
        end
      else
        ext ||= (rb ? '.rb' : '.so')
        if @features.include?(fname + ext)
          return ext == '.rb' ? '.rb' : '.so'
        end
      end
      nil
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
