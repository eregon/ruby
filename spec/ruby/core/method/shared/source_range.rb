require_relative '../fixtures/classes'

describe :method_source_range, shared: true do
  before :each do
    @fixtures_path = File.realpath('../fixtures/classes.rb', __dir__)
  end

  it "returns a Ruby::SourceRange" do
    method = @object.call(MethodSpecs::SourceLocation.method(:location))

    method.source_range.should be_an_instance_of(Ruby::SourceRange)
  end

  it "sets absolute_path to the real path of the source file" do
    method = @object.call(MethodSpecs::SourceLocation.method(:location))

    method.source_range.absolute_path.should == @fixtures_path
  end

  it "sets path to the source location path" do
    method = @object.call(MethodSpecs::SourceLocation.method(:location))

    method.source_range.path.should == @fixtures_path
  end

  it "sets the start and end lines and columns" do
    method = @object.call(MethodSpecs::SourceLocation.method(:location))
    range = method.source_range

    [range.start_line, range.start_column, range.end_line, range.end_column].should == MethodSpecs::SourceLocation::LOCATION_SOURCE_RANGE
  end

  it "works for multi-line methods" do
    method = @object.call(MethodSpecs::SourceLocation.method(:multiline))
    range = method.source_range

    range.end_line.should > range.start_line
    [range.start_line, range.start_column, range.end_line, range.end_column].should == MethodSpecs::SourceLocation::MULTILINE_SOURCE_RANGE
  end

  it "works for UTF-8 method names" do
    method = @object.call(MethodSpecs::SourceLocation.new.method(:été))
    range = method.source_range

    [range.start_line, range.start_column, range.end_line, range.end_column].should == MethodSpecs::SourceLocation::UTF8_SOURCE_RANGE
  end

  it "works for inline methods" do
    method = @object.call(MethodSpecs::SourceLocation.new.method(:inline))
    range = method.source_range

    [range.start_line, range.start_column, range.end_line, range.end_column].should == MethodSpecs::SourceLocation::INLINE_SOURCE_RANGE
  end

  it "works for methods defined with define_method" do
    method = @object.call(MethodSpecs::SourceLocation.new.method(:define_method_method))
    range = method.source_range

    [range.start_line, range.start_column, range.end_line, range.end_column].should == MethodSpecs::SourceLocation::DEFINE_METHOD_SOURCE_RANGE
  end

  it "returns nil for core methods" do
    method = @object.call("".method(:length))

    method.source_range.should == nil
  end

  it "sets path when absolute_path is nil" do
    c = Class.new do
      eval('def self.m; end', nil, "foo", 100)
    end
    method = @object.call(c.method(:m))
    range = method.source_range

    range.path.should == "foo"
    range.absolute_path.should == nil
    range.start_line.should == 100
  end
end
