require_relative '../../spec_helper'
require_relative 'fixtures/source_location'

ruby_version_is "4.1" do
  describe "Proc#source_range" do
    def source_range_values(range)
      [range.start_line, range.start_column, range.end_line, range.end_column]
    end

    before :each do
      @proc = ProcSpecs::SourceLocation.my_proc
    end

    it "returns a Ruby::SourceRange" do
      @proc.source_range.should be_an_instance_of(Ruby::SourceRange)
    end

    it "sets absolute_path to the real path of the source file" do
      @proc.source_range.absolute_path.should == File.realpath('fixtures/source_location.rb', __dir__)
    end

    it "sets path to the source location path" do
      @proc.source_range.path.should == File.realpath('fixtures/source_location.rb', __dir__)
    end

    it "sets the start and end lines and columns" do
      source_range_values(@proc.source_range).should == ProcSpecs::SourceLocation::MY_PROC_SOURCE_RANGE
    end

    it "works for multi-line procs" do
      range = ProcSpecs::SourceLocation.my_multiline_proc.source_range
      source_range_values(range).should == ProcSpecs::SourceLocation::MY_MULTILINE_PROC_SOURCE_RANGE
    end

    it "works for lambda calls" do
      range = ProcSpecs::SourceLocation.my_block_lambda.source_range
      source_range_values(range).should == ProcSpecs::SourceLocation::MY_BLOCK_LAMBDA_SOURCE_RANGE
    end

    it "works for returned blocks" do
      range = ProcSpecs::SourceLocation.my_returned_block.source_range
      source_range_values(range).should == ProcSpecs::SourceLocation::MY_RETURNED_BLOCK_SOURCE_RANGE
    end

    it "works for blocks passed to calls with receivers" do
      range = ProcSpecs::SourceLocation.my_receiver_block.source_range
      source_range_values(range).should == ProcSpecs::SourceLocation::MY_RECEIVER_BLOCK_SOURCE_RANGE
    end

    it "works for heredoc procs" do
      range = ProcSpecs::SourceLocation.my_heredoc_proc.source_range
      source_range_values(range).should == ProcSpecs::SourceLocation::MY_HEREDOC_PROC_SOURCE_RANGE
    end

    it "works for for-loop body procs" do
      range = ProcSpecs::SourceLocation.my_for_body_proc.source_range
      source_range_values(range).should == ProcSpecs::SourceLocation::MY_FOR_BODY_PROC_SOURCE_RANGE
    end

    it "returns the same range for a proc-ified method as the method reports" do
      method = ProcSpecs::SourceLocation.method(:my_proc)
      proc = method.to_proc

      source_range_values(proc.source_range).should == source_range_values(method.source_range)
      proc.source_range.absolute_path.should == method.source_range.absolute_path
    end

    it "returns nil for a core method that has been proc-ified" do
      [].method(:<<).to_proc.source_range.should == nil
    end

    it "sets path when absolute_path is nil" do
      range = eval('-> { 1 }', nil, "foo", 100).source_range

      range.path.should == "foo"
      range.absolute_path.should == nil
      range.start_line.should == 100
    end
  end
end
