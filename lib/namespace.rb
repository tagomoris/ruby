require 'tmpdir'
require 'fileutils'
require 'rubygems'

$MAIN = self

module NameSpace
  NAMESPACE_TMP_DIR = Dir.tmpdir

  def self.define
    ns = Module.new
    ns.extend NameSpaceMethods
    ns.instance_variable_set(:@features, [])
    ns.instance_variable_set(:@realpaths, {}) # realpath => Qtrue
    ns.instance_variable_set(:@realpath_map, {}) # feature => realpath
    ns.instance_variable_set(:@ext_handles, {}) # name => handle
    ns.instance_variable_set(:@LOAD_PATH, $LOAD_PATH.dup)
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

    def ext_name_in_namespace(path)
      newname = self.object_id.to_s + '_' + File.basename(path)
      newpath = File.join(NAMESPACE_TMP_DIR, newname)
      FileUtils.cp(path, newpath, preserve: true)
      newpath
    end

    private def gem_dirs(fname, version)
      base = Gem.dir
      arch_name = Dir.open(File.join(base, 'extensions')){|dir| dir.children }.first
      ext_dir_name = Dir.open(File.join(base, 'extensions', arch_name)){|dir| dir.children }.first
      gem_dir = "#{fname}-#{version}"
      [
        File.join(base, 'extensions', arch_name, ext_dir_name, gem_dir),
        File.join(base, 'gems', gem_dir, 'lib'),
      ]
    end

    def require(fname, version: nil)
      if version
        gem_dirs(fname, version).each do |dir|
          @LOAD_PATH.unshift dir
        end
      end

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
