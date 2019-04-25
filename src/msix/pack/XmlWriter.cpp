//
//  Copyright (C) 2019 Microsoft.  All rights reserved.
//  See LICENSE file in the project root for full license information.
//
#include "XmlWriter.hpp"
#include "StringStream.hpp"
#include "ComHelper.hpp"
#include "Exceptions.hpp"
#include "MsixErrors.hpp"
#include "StreamHelper.hpp"

#include <stack>
#include <string>

namespace MSIX {

        const static char* xmlStart = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"true\"?><";

        // Adds xml header declaration plus the name of the root element
        void XmlWriter::StartWrite(const std::string& root)
        {
            m_elements.emplace(root);
            Write(xmlStart);
            Write(root);
            m_state = State::OpenElement;
        }

        void XmlWriter::StartElement(const std::string& name)
        {
            ThrowErrorIf(Error::XmlError, m_state == State::Finish, "Invalid call, xml already finished");
            m_elements.emplace(name);
            // If the state is open, then we are adding a child element to the previous one. We need to close that element's
            // tag and add the new one. If the state is closed, there is no need to close the tag as it had already been closed.
            if (m_state == State::OpenElement)
            {
                Write(">"); // close parent element
            }
            Write("<");
            Write(name);
            m_state = State::OpenElement;
        }

        void XmlWriter::CloseElement()
        {
            ThrowErrorIf(Error::XmlError, m_state == State::Finish, "Invalid call, xml already finished");
            // If the state is open and we are closing an element, it means that it doesn't have any child, so we can
            // just close it with "/>". If we are closing an element and a closing just happened, it means that we are 
            // closing an element that has child elements, so it must be closed with </element>
            if (m_state == State::OpenElement)
            {
                Write("/>");
            }
            else // State::ClosedElement
            {
                // </name>
                Write("</");
                Write(m_elements.top());
                Write(">");
            }
            m_state = State::ClosedElement;
            m_elements.pop();
            if (m_elements.size() == 0)
            {
                m_state = State::Finish;
            }
        }

        void XmlWriter::AddAttribute(const std::string& name, const std::string& value)
        {
            ThrowErrorIf(Error::XmlError, (m_state == State::Finish) || (m_state == State::ClosedElement), "Invalid call to AddAttribute");
            Write(" "); // always write a space. We just wrote either an element or an attribute
            Write(name); // name="value"
            Write("=\"");
            Write(value);
            Write("\"");
        }

        ComPtr<IStream> XmlWriter::GetStream()
        {
            ThrowErrorIf(Error::XmlError, m_state == State::Finish, "Invalid call, the stream can only be accessed when the writer is done");
            return m_stream;
        }

        void XmlWriter::Write(const std::string& toWrite)
        {
            Helper::WriteStringToStream(m_stream, toWrite);
        }
}
