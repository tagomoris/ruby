# frozen_string_literal: true
#
# DO NOT MODIFY!!!!
# This file is automatically generated by Racc 1.4.14
# from Racc grammer file "".
#

require 'racc/parser.rb'

class RDoc::RD

##
# RD format parser for headings, paragraphs, lists, verbatim sections that
# exist as blocks.

class BlockParser < Racc::Parser


# :stopdoc:

TMPFILE = ["rdtmp", $$, 0]

MARK_TO_LEVEL = {
  '='    => 1,
  '=='   => 2,
  '==='  => 3,
  '====' => 4,
  '+'    => 5,
  '++'   => 6,
}

# :startdoc:

##
# Footnotes for this document

attr_reader :footnotes

##
# Labels for items in this document

attr_reader :labels

##
# Path to find included files in

attr_accessor :include_path

##
# Creates a new RDoc::RD::BlockParser.  Use #parse to parse an rd-format
# document.

def initialize
  @inline_parser = RDoc::RD::InlineParser.new self
  @include_path = []

  # for testing
  @footnotes = []
  @labels    = {}
end

##
# Parses +src+ and returns an RDoc::Markup::Document.

def parse src
  @src = src
  @src.push false

  @footnotes = []
  @labels    = {}

  # @i: index(line no.) of src
  @i = 0

  # stack for current indentation
  @indent_stack = []

  # how indented.
  @current_indent = @indent_stack.join("")

  # RDoc::RD::BlockParser for tmp src
  @subparser = nil

  # which part is in now
  @in_part = nil
  @part_content = []

  @in_verbatim = false

  @yydebug = true

  document = do_parse

  unless @footnotes.empty? then
    blankline = document.parts.pop

    document.parts << RDoc::Markup::Rule.new(1)
    document.parts.concat @footnotes

    document.parts.push blankline
  end

  document
end

##
# Returns the next token from the document

def next_token # :nodoc:
  # preprocessing
  # if it is not in RD part
  # => method
  while @in_part != "rd"
    line = @src[@i]
    @i += 1 # next line

    case line
    # src end
    when false
      return [false, false]
    # RD part begin
    when /^=begin\s*(?:\bRD\b.*)?\s*$/
      if @in_part # if in non-RD part
        @part_content.push(line)
      else
        @in_part = "rd"
        return [:WHITELINE, "=begin\n"] # <= for textblockand
      end
    # non-RD part begin
    when /^=begin\s+(\w+)/
      part = $1
      if @in_part # if in non-RD part
        @part_content.push(line)
      else
        @in_part = part if @tree.filter[part] # if filter exists
#  p "BEGIN_PART: #{@in_part}" # DEBUG
      end
    # non-RD part end
    when /^=end/
      if @in_part # if in non-RD part
#  p "END_PART: #{@in_part}" # DEBUG
        # make Part-in object
        part = RDoc::RD::Part.new(@part_content.join(""), @tree, "r")
        @part_content.clear
        # call filter, part_out is output(Part object)
        part_out = @tree.filter[@in_part].call(part)

        if @tree.filter[@in_part].mode == :rd # if output is RD formatted
          subtree = parse_subtree(part_out.to_a)
        else # if output is target formatted
          basename = TMPFILE.join('.')
          TMPFILE[-1] += 1
          tmpfile = open(@tree.tmp_dir + "/" + basename + ".#{@in_part}", "w")
          tmpfile.print(part_out)
          tmpfile.close
          subtree = parse_subtree(["=begin\n", "<<< #{basename}\n", "=end\n"])
        end
        @in_part = nil
        return [:SUBTREE, subtree]
      end
    else
      if @in_part # if in non-RD part
        @part_content.push(line)
      end
    end
  end

  @current_indent = @indent_stack.join("")
  line = @src[@i]
  case line
  when false
    if_current_indent_equal("") do
      [false, false]
    end
  when /^=end/
    if_current_indent_equal("") do
      @in_part = nil
      [:WHITELINE, "=end"] # MUST CHANGE??
    end
  when /^\s*$/
    @i += 1 # next line
    return [:WHITELINE, ':WHITELINE']
  when /^\#/  # comment line
    @i += 1 # next line
    self.next_token()
  when /^(={1,4})(?!=)\s*(?=\S)/, /^(\+{1,2})(?!\+)\s*(?=\S)/
    rest = $'                    # '
    rest.strip!
    mark = $1
    if_current_indent_equal("") do
      return [:HEADLINE, [MARK_TO_LEVEL[mark], rest]]
    end
  when /^<<<\s*(\S+)/
    file = $1
    if_current_indent_equal("") do
      suffix = file[-3 .. -1]
      if suffix == ".rd" or suffix == ".rb"
        subtree = parse_subtree(get_included(file))
        [:SUBTREE, subtree]
      else
        [:INCLUDE, file]
      end
    end
  when /^(\s*)\*(\s*)/
    rest = $'                   # '
    newIndent = $2
    if_current_indent_equal($1) do
      if @in_verbatim
        [:STRINGLINE, line]
      else
        @indent_stack.push("\s" + newIndent)
        [:ITEMLISTLINE, rest]
      end
    end
  when /^(\s*)(\(\d+\))(\s*)/
    rest = $'                     # '
    mark = $2
    newIndent = $3
    if_current_indent_equal($1) do
      if @in_verbatim
        [:STRINGLINE, line]
      else
        @indent_stack.push("\s" * mark.size + newIndent)
        [:ENUMLISTLINE, rest]
      end
    end
  when /^(\s*):(\s*)/
    rest = $'                    # '
    newIndent = $2
    if_current_indent_equal($1) do
      if @in_verbatim
        [:STRINGLINE, line]
      else
        @indent_stack.push("\s#{$2}")
        [:DESCLISTLINE, rest]
      end
    end
  when /^(\s*)---(?!-|\s*$)/
    indent = $1
    rest = $'
    /\s*/ === rest
    term = $'
    new_indent = $&
    if_current_indent_equal(indent) do
      if @in_verbatim
        [:STRINGLINE, line]
      else
        @indent_stack.push("\s\s\s" + new_indent)
        [:METHODLISTLINE, term]
      end
    end
  when /^(\s*)/
    if_current_indent_equal($1) do
      [:STRINGLINE, line]
    end
  else
    raise "[BUG] parsing error may occurred."
  end
end

##
# Yields to the given block if +indent+ matches the current indent, otherwise
# an indentation token is processed.

def if_current_indent_equal(indent)
  indent = indent.sub(/\t/, "\s" * 8)
  if @current_indent == indent
    @i += 1 # next line
    yield
  elsif indent.index(@current_indent) == 0
    @indent_stack.push(indent[@current_indent.size .. -1])
    [:INDENT, ":INDENT"]
  else
    @indent_stack.pop
    [:DEDENT, ":DEDENT"]
  end
end
private :if_current_indent_equal

##
# Cuts off excess whitespace in +src+

def cut_off(src)
  ret = []
  whiteline_buf = []

  line = src.shift
  /^\s*/ =~ line

  indent = Regexp.quote($&)
  ret.push($')

  while line = src.shift
    if /^(\s*)$/ =~ line
      whiteline_buf.push(line)
    elsif /^#{indent}/ =~ line
      unless whiteline_buf.empty?
        ret.concat(whiteline_buf)
        whiteline_buf.clear
      end
      ret.push($')
    else
      raise "[BUG]: probably Parser Error while cutting off.\n"
    end
  end
  ret
end
private :cut_off

def set_term_to_element(parent, term)
#  parent.set_term_under_document_struct(term, @tree.document_struct)
  parent.set_term_without_document_struct(term)
end
private :set_term_to_element

##
# Raises a ParseError when invalid formatting is found

def on_error(et, ev, _values)
  prv, cur, nxt = format_line_num(@i, @i+1, @i+2)

  raise ParseError, <<Msg

RD syntax error: line #{@i+1}:
  #{prv}  |#{@src[@i-1].chomp}
  #{cur}=>|#{@src[@i].chomp}
  #{nxt}  |#{@src[@i+1].chomp}

Msg
end

##
# Current line number

def line_index
  @i
end

##
# Parses subtree +src+

def parse_subtree src
  @subparser ||= RDoc::RD::BlockParser.new

  @subparser.parse src
end
private :parse_subtree

##
# Retrieves the content for +file+ from the include_path

def get_included(file)
  included = []

  @include_path.each do |dir|
    file_name = File.join dir, file

    if File.exist? file_name then
      included = IO.readlines file_name
      break
    end
  end

  included
end
private :get_included

##
# Formats line numbers +line_numbers+ prettily

def format_line_num(*line_numbers)
  width = line_numbers.collect{|i| i.to_s.length }.max
  line_numbers.collect{|i| sprintf("%#{width}d", i) }
end
private :format_line_num

##
# Retrieves the content of +values+ as a single String

def content values
 values.map { |value| value.content }.join
end

##
# Creates a paragraph for +value+

def paragraph value
  content = cut_off(value).join(' ').rstrip
  contents = @inline_parser.parse content

  RDoc::Markup::Paragraph.new(*contents)
end

##
# Adds footnote +content+ to the document

def add_footnote content
  index = @footnotes.length / 2 + 1

  footmark_link = "{^#{index}}[rdoc-label:footmark-#{index}:foottext-#{index}]"

  @footnotes << RDoc::Markup::Paragraph.new(footmark_link, ' ', *content)
  @footnotes << RDoc::Markup::BlankLine.new

  index
end

##
# Adds label +label+ to the document

def add_label label
  @labels[label] = true

  label
end

# :stopdoc:

##### State transition tables begin ###

racc_action_table = [
    34,    35,    30,    33,    40,    34,    35,    30,    33,    40,
    65,    34,    35,    30,    33,    14,    73,    14,    54,    76,
    15,    88,    34,    35,    30,    33,    14,    73,    77,    33,
    54,    15,    34,    35,    30,    33,    14,    73,    81,    38,
    38,    15,    34,    35,    30,    33,    14,    73,    40,    36,
    83,    15,    34,    35,    30,    33,    54,    47,    30,    35,
    34,    15,    34,    35,    30,    33,    14,    73,    38,    67,
    59,    15,    34,    35,    30,    33,    14,     9,    10,    11,
    12,    15,    34,    35,    30,    33,    14,    73,    14,   nil,
   nil,    15,    34,    35,    30,    33,    14,    73,   nil,   nil,
   nil,    15,    34,    35,    30,    33,   nil,    47,   nil,   nil,
   nil,    15,    34,    35,    30,    33,    14,    73,   nil,   nil,
   nil,    15,    34,    35,    30,    33,    14,    73,   nil,   nil,
   nil,    15,    34,    35,    30,    33,    14,     9,    10,    11,
    12,    15,    34,    35,    30,    33,    14,    73,    61,    63,
   nil,    15,    14,    62,    60,    61,    63,    79,    61,    63,
    62,    87,   nil,    62,    34,    35,    30,    33 ]

racc_action_check = [
    41,    41,    41,    41,    41,    15,    15,    15,    15,    15,
    41,    86,    86,    86,    86,    86,    86,    34,    33,    49,
    86,    86,    85,    85,    85,    85,    85,    85,    51,    31,
    54,    85,    79,    79,    79,    79,    79,    79,    56,    57,
    58,    79,    78,    78,    78,    78,    78,    78,    62,     1,
    66,    78,    24,    24,    24,    24,    30,    24,    28,    25,
    22,    24,    75,    75,    75,    75,    75,    75,    13,    44,
    36,    75,     2,     2,     2,     2,     2,     2,     2,     2,
     2,     2,    46,    46,    46,    46,    46,    46,    35,   nil,
   nil,    46,    45,    45,    45,    45,    45,    45,   nil,   nil,
   nil,    45,    27,    27,    27,    27,   nil,    27,   nil,   nil,
   nil,    27,    74,    74,    74,    74,    74,    74,   nil,   nil,
   nil,    74,    68,    68,    68,    68,    68,    68,   nil,   nil,
   nil,    68,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,    47,    47,    47,    47,    47,    47,    39,    39,
   nil,    47,    52,    39,    39,    82,    82,    52,    64,    64,
    82,    82,   nil,    64,    20,    20,    20,    20 ]

racc_action_pointer = [
   129,    49,    69,   nil,   nil,   nil,   nil,   nil,   nil,   nil,
   nil,   nil,   nil,    61,   nil,     2,   nil,   nil,   nil,   nil,
   161,   nil,    57,   nil,    49,    55,   nil,    99,    53,   nil,
    48,    23,   nil,    10,    10,    81,    70,   nil,   nil,   141,
   nil,    -3,   nil,   nil,    56,    89,    79,   139,   nil,     6,
   nil,    15,   145,   nil,    22,   nil,    25,    32,    33,   nil,
   nil,   nil,    41,   nil,   151,   nil,    37,   nil,   119,   nil,
   nil,   nil,   nil,   nil,   109,    59,   nil,   nil,    39,    29,
   nil,   nil,   148,   nil,   nil,    19,     8,   nil,   nil ]

racc_action_default = [
    -2,   -73,    -1,    -4,    -5,    -6,    -7,    -8,    -9,   -10,
   -11,   -12,   -13,   -14,   -16,   -73,   -23,   -24,   -25,   -26,
   -27,   -31,   -32,   -34,   -72,   -36,   -38,   -72,   -40,   -42,
   -59,   -44,   -46,   -59,   -63,   -65,   -73,    -3,   -15,   -73,
   -22,   -73,   -30,   -33,   -73,   -69,   -70,   -71,   -37,   -73,
   -41,   -73,   -51,   -58,   -61,   -45,   -73,   -62,   -64,    89,
   -17,   -19,   -73,   -21,   -18,   -28,   -73,   -35,   -66,   -53,
   -54,   -55,   -56,   -57,   -67,   -68,   -39,   -43,   -49,   -73,
   -60,   -47,   -73,   -29,   -52,   -48,   -73,   -20,   -50 ]

racc_goto_table = [
     4,    39,     4,    68,    74,    75,     6,     5,     6,     5,
    44,    42,    51,    49,     3,    56,    37,    57,    58,    80,
     2,    66,    84,    41,    43,    48,    50,    64,    84,    84,
    46,    45,    42,    46,    45,    55,    85,    86,     1,    84,
    84,   nil,   nil,   nil,   nil,   nil,   nil,   nil,    82,   nil,
   nil,   nil,    78 ]

racc_goto_check = [
     4,    10,     4,    31,    31,    31,     6,     5,     6,     5,
    21,    12,    27,    21,     3,    27,     3,     9,     9,    33,
     2,    11,    32,    17,    19,    23,    26,    10,    32,    32,
     6,     5,    12,     6,     5,    29,    31,    31,     1,    32,
    32,   nil,   nil,   nil,   nil,   nil,   nil,   nil,    10,   nil,
   nil,   nil,     4 ]

racc_goto_pointer = [
   nil,    38,    20,    14,     0,     7,     6,   nil,   nil,   -17,
   -14,   -20,    -9,   nil,   nil,   nil,   nil,     8,   nil,     2,
   nil,   -14,   nil,     0,   nil,   nil,    -2,   -18,   nil,     4,
   nil,   -42,   -46,   -35 ]

racc_goto_default = [
   nil,   nil,   nil,   nil,    70,    71,    72,     7,     8,    13,
   nil,   nil,    21,    16,    17,    18,    19,    20,    22,    23,
    24,   nil,    25,    26,    27,    28,    29,   nil,    31,    32,
    52,   nil,    69,    53 ]

racc_reduce_table = [
  0, 0, :racc_error,
  1, 15, :_reduce_1,
  0, 15, :_reduce_2,
  2, 16, :_reduce_3,
  1, 16, :_reduce_4,
  1, 17, :_reduce_5,
  1, 17, :_reduce_6,
  1, 17, :_reduce_none,
  1, 17, :_reduce_8,
  1, 17, :_reduce_9,
  1, 17, :_reduce_10,
  1, 17, :_reduce_11,
  1, 21, :_reduce_12,
  1, 22, :_reduce_13,
  1, 18, :_reduce_14,
  2, 23, :_reduce_15,
  1, 23, :_reduce_16,
  3, 19, :_reduce_17,
  1, 25, :_reduce_18,
  2, 24, :_reduce_19,
  4, 24, :_reduce_20,
  2, 24, :_reduce_21,
  1, 24, :_reduce_22,
  1, 26, :_reduce_none,
  1, 26, :_reduce_none,
  1, 26, :_reduce_none,
  1, 26, :_reduce_none,
  1, 20, :_reduce_27,
  3, 20, :_reduce_28,
  4, 20, :_reduce_29,
  2, 31, :_reduce_30,
  1, 31, :_reduce_31,
  1, 27, :_reduce_32,
  2, 32, :_reduce_33,
  1, 32, :_reduce_34,
  3, 33, :_reduce_35,
  1, 28, :_reduce_36,
  2, 36, :_reduce_37,
  1, 36, :_reduce_38,
  3, 37, :_reduce_39,
  1, 29, :_reduce_40,
  2, 39, :_reduce_41,
  1, 39, :_reduce_42,
  3, 40, :_reduce_43,
  1, 30, :_reduce_44,
  2, 42, :_reduce_45,
  1, 42, :_reduce_46,
  3, 43, :_reduce_47,
  3, 41, :_reduce_48,
  2, 41, :_reduce_49,
  4, 41, :_reduce_50,
  1, 41, :_reduce_51,
  2, 45, :_reduce_52,
  1, 45, :_reduce_none,
  1, 46, :_reduce_54,
  1, 46, :_reduce_55,
  1, 46, :_reduce_none,
  1, 46, :_reduce_57,
  1, 44, :_reduce_none,
  0, 44, :_reduce_none,
  2, 47, :_reduce_none,
  1, 47, :_reduce_none,
  2, 34, :_reduce_62,
  1, 34, :_reduce_63,
  2, 38, :_reduce_64,
  1, 38, :_reduce_65,
  2, 35, :_reduce_66,
  2, 35, :_reduce_67,
  2, 35, :_reduce_68,
  1, 35, :_reduce_69,
  1, 35, :_reduce_none,
  1, 35, :_reduce_71,
  0, 35, :_reduce_72 ]

racc_reduce_n = 73

racc_shift_n = 89

racc_token_table = {
  false => 0,
  :error => 1,
  :DUMMY => 2,
  :ITEMLISTLINE => 3,
  :ENUMLISTLINE => 4,
  :DESCLISTLINE => 5,
  :METHODLISTLINE => 6,
  :STRINGLINE => 7,
  :WHITELINE => 8,
  :SUBTREE => 9,
  :HEADLINE => 10,
  :INCLUDE => 11,
  :INDENT => 12,
  :DEDENT => 13 }

racc_nt_base = 14

racc_use_result_var = true

Racc_arg = [
  racc_action_table,
  racc_action_check,
  racc_action_default,
  racc_action_pointer,
  racc_goto_table,
  racc_goto_check,
  racc_goto_default,
  racc_goto_pointer,
  racc_nt_base,
  racc_reduce_table,
  racc_token_table,
  racc_shift_n,
  racc_reduce_n,
  racc_use_result_var ]

Racc_token_to_s_table = [
  "$end",
  "error",
  "DUMMY",
  "ITEMLISTLINE",
  "ENUMLISTLINE",
  "DESCLISTLINE",
  "METHODLISTLINE",
  "STRINGLINE",
  "WHITELINE",
  "SUBTREE",
  "HEADLINE",
  "INCLUDE",
  "INDENT",
  "DEDENT",
  "$start",
  "document",
  "blocks",
  "block",
  "textblock",
  "verbatim",
  "lists",
  "headline",
  "include",
  "textblockcontent",
  "verbatimcontent",
  "verbatim_after_lists",
  "list",
  "itemlist",
  "enumlist",
  "desclist",
  "methodlist",
  "lists2",
  "itemlistitems",
  "itemlistitem",
  "first_textblock_in_itemlist",
  "other_blocks_in_list",
  "enumlistitems",
  "enumlistitem",
  "first_textblock_in_enumlist",
  "desclistitems",
  "desclistitem",
  "description_part",
  "methodlistitems",
  "methodlistitem",
  "whitelines",
  "blocks_in_list",
  "block_in_list",
  "whitelines2" ]

Racc_debug_parser = false

##### State transition tables end #####

# reduce 0 omitted

def _reduce_1(val, _values, result)
 result = RDoc::Markup::Document.new(*val[0]) 
    result
end

def _reduce_2(val, _values, result)
 raise ParseError, "file empty" 
    result
end

def _reduce_3(val, _values, result)
 result = val[0].concat val[1] 
    result
end

def _reduce_4(val, _values, result)
 result = val[0] 
    result
end

def _reduce_5(val, _values, result)
 result = val 
    result
end

def _reduce_6(val, _values, result)
 result = val 
    result
end

# reduce 7 omitted

def _reduce_8(val, _values, result)
 result = val 
    result
end

def _reduce_9(val, _values, result)
 result = val 
    result
end

def _reduce_10(val, _values, result)
 result = [RDoc::Markup::BlankLine.new] 
    result
end

def _reduce_11(val, _values, result)
 result = val[0].parts 
    result
end

def _reduce_12(val, _values, result)
      # val[0] is like [level, title]
      title = @inline_parser.parse(val[0][1])
      result = RDoc::Markup::Heading.new(val[0][0], title)
    
    result
end

def _reduce_13(val, _values, result)
      result = RDoc::Markup::Include.new val[0], @include_path
    
    result
end

def _reduce_14(val, _values, result)
      # val[0] is Array of String
      result = paragraph val[0]
    
    result
end

def _reduce_15(val, _values, result)
 result << val[1].rstrip 
    result
end

def _reduce_16(val, _values, result)
 result = [val[0].rstrip] 
    result
end

def _reduce_17(val, _values, result)
      # val[1] is Array of String
      content = cut_off val[1]
      result = RDoc::Markup::Verbatim.new(*content)

      # imform to lexer.
      @in_verbatim = false
    
    result
end

def _reduce_18(val, _values, result)
      # val[0] is Array of String
      content = cut_off val[0]
      result = RDoc::Markup::Verbatim.new(*content)

      # imform to lexer.
      @in_verbatim = false
    
    result
end

def _reduce_19(val, _values, result)
      result << val[1]
    
    result
end

def _reduce_20(val, _values, result)
      result.concat val[2]
    
    result
end

def _reduce_21(val, _values, result)
      result << "\n"
    
    result
end

def _reduce_22(val, _values, result)
      result = val
      # inform to lexer.
      @in_verbatim = true
    
    result
end

# reduce 23 omitted

# reduce 24 omitted

# reduce 25 omitted

# reduce 26 omitted

def _reduce_27(val, _values, result)
      result = val[0]
    
    result
end

def _reduce_28(val, _values, result)
      result = val[1]
    
    result
end

def _reduce_29(val, _values, result)
      result = val[1].push(val[2])
    
    result
end

def _reduce_30(val, _values, result)
 result = val[0] << val[1] 
    result
end

def _reduce_31(val, _values, result)
 result = [val[0]] 
    result
end

def _reduce_32(val, _values, result)
      result = RDoc::Markup::List.new :BULLET, *val[0]
    
    result
end

def _reduce_33(val, _values, result)
 result.push(val[1]) 
    result
end

def _reduce_34(val, _values, result)
 result = val 
    result
end

def _reduce_35(val, _values, result)
      result = RDoc::Markup::ListItem.new nil, val[0], *val[1]
    
    result
end

def _reduce_36(val, _values, result)
      result = RDoc::Markup::List.new :NUMBER, *val[0]
    
    result
end

def _reduce_37(val, _values, result)
 result.push(val[1]) 
    result
end

def _reduce_38(val, _values, result)
 result = val 
    result
end

def _reduce_39(val, _values, result)
      result = RDoc::Markup::ListItem.new nil, val[0], *val[1]
    
    result
end

def _reduce_40(val, _values, result)
      result = RDoc::Markup::List.new :NOTE, *val[0]
    
    result
end

def _reduce_41(val, _values, result)
 result.push(val[1]) 
    result
end

def _reduce_42(val, _values, result)
 result = val 
    result
end

def _reduce_43(val, _values, result)
      term = @inline_parser.parse val[0].strip

      result = RDoc::Markup::ListItem.new term, *val[1]
    
    result
end

def _reduce_44(val, _values, result)
      result = RDoc::Markup::List.new :LABEL, *val[0]
    
    result
end

def _reduce_45(val, _values, result)
 result.push(val[1]) 
    result
end

def _reduce_46(val, _values, result)
 result = val 
    result
end

def _reduce_47(val, _values, result)
      result = RDoc::Markup::ListItem.new "<tt>#{val[0].strip}</tt>", *val[1]
    
    result
end

def _reduce_48(val, _values, result)
      result = [val[1]].concat(val[2])
    
    result
end

def _reduce_49(val, _values, result)
      result = [val[1]]
    
    result
end

def _reduce_50(val, _values, result)
      result = val[2]
    
    result
end

def _reduce_51(val, _values, result)
      result = []
    
    result
end

def _reduce_52(val, _values, result)
 result.concat val[1] 
    result
end

# reduce 53 omitted

def _reduce_54(val, _values, result)
 result = val 
    result
end

def _reduce_55(val, _values, result)
 result = val 
    result
end

# reduce 56 omitted

def _reduce_57(val, _values, result)
 result = [] 
    result
end

# reduce 58 omitted

# reduce 59 omitted

# reduce 60 omitted

# reduce 61 omitted

def _reduce_62(val, _values, result)
      result = paragraph [val[0]].concat(val[1])
    
    result
end

def _reduce_63(val, _values, result)
      result = paragraph [val[0]]
    
    result
end

def _reduce_64(val, _values, result)
      result = paragraph [val[0]].concat(val[1])
    
    result
end

def _reduce_65(val, _values, result)
      result = paragraph [val[0]]
    
    result
end

def _reduce_66(val, _values, result)
      result = [val[0]].concat(val[1])
    
    result
end

def _reduce_67(val, _values, result)
 result.concat val[1] 
    result
end

def _reduce_68(val, _values, result)
 result = val[1] 
    result
end

def _reduce_69(val, _values, result)
 result = val 
    result
end

# reduce 70 omitted

def _reduce_71(val, _values, result)
 result = [] 
    result
end

def _reduce_72(val, _values, result)
 result = [] 
    result
end

def _reduce_none(val, _values, result)
  val[0]
end

end   # class BlockParser

end
