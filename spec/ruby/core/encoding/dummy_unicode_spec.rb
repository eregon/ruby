require_relative '../../spec_helper'

describe "Encoding::UTF_16" do
  ruby_version_is ""..."3.3" do
    it "is a dummy encoding" do
      suppress_warning { Encoding::UTF_16 }.should.dummy?
    end
  end

  ruby_version_is "3.2"..."3.3" do
    it "is deprecated" do
      -> { Encoding::UTF_16 }.should complain(/Encoding::UTF_16 is deprecated/)
    end
  end

  ruby_version_is "3.3" do
    it "has been removed" do
      Encoding.should_not have_constant(:UTF_16)
    end
  end
end

describe "Encoding::UTF_32" do
  ruby_version_is ""..."3.3" do
    it "is a dummy encoding" do
      suppress_warning { Encoding::UTF_32 }.should.dummy?
    end
  end

  ruby_version_is "3.2"..."3.3" do
    it "is deprecated" do
      -> { Encoding::UTF_32 }.should complain(/Encoding::UTF_32 is deprecated/)
    end
  end

  ruby_version_is "3.3" do
    it "has been removed" do
      Encoding.should_not have_constant(:UTF_32)
    end
  end
end
