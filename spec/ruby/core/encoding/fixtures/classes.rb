# -*- encoding: binary -*-
module EncodingSpecs
  # Avoids deprecation warnings on 3.2 for the dummy UTF-16/UTF-32
  def self.find(name)
    case name
    when 'UTF-16', 'UTF-32'
      suppress_warning { Encoding.find(name) }
    else
      Encoding.find(name)
    end
  end

  class UndefinedConversionError
    def self.exception
      ec = Encoding::Converter.new('utf-8','ascii')
      begin
        ec.convert("\u{8765}")
      rescue Encoding::UndefinedConversionError => e
        e
      end
    end
  end

  class UndefinedConversionErrorIndirect
    def self.exception
      ec = Encoding::Converter.new("ISO-8859-1", "EUC-JP")
      begin
        ec.convert("\xA0")
      rescue Encoding::UndefinedConversionError => e
        e
      end
    end
  end

  class InvalidByteSequenceError
    def self.exception
      ec = Encoding::Converter.new("utf-8", "iso-8859-1")
      begin
        ec.convert("\xf1abcd")
      rescue Encoding::InvalidByteSequenceError => e
        # Return the exception object and the primitive_errinfo Array
        [e, ec.primitive_errinfo]
      end
    end
  end

  class InvalidByteSequenceErrorIndirect
    def self.exception
      ec = Encoding::Converter.new("EUC-JP", "ISO-8859-1")
      begin
        ec.convert("abc\xA1\xFFdef")
      rescue Encoding::InvalidByteSequenceError => e
        # Return the exception object and the discarded bytes reported by
        # #primitive_errinfo
        [e, ec.primitive_errinfo]
      end
    end
  end
end
