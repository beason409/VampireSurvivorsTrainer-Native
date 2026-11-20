using System.Text;

namespace Trainer.Desktop;

// StreamReader.ReadLineAsync materializes the whole line before the caller can
// check its length. Bound allocation while reading, retaining chunked I/O so
// icon/catalog responses do not require one async operation per character.
internal sealed class BoundedResponseReader(StreamReader reader, int maximumCharacters)
{
    private readonly char[] _buffer = new char[4096];
    private int _position, _count;

    internal async Task<string?> ReadLineAsync(CancellationToken cancellationToken)
    {
        var text = new StringBuilder();
        while (true)
        {
            cancellationToken.ThrowIfCancellationRequested();
            if (_position == _count)
            {
                _count = await reader.ReadAsync(_buffer.AsMemory(), cancellationToken).ConfigureAwait(false);
                _position = 0;
                if (_count == 0)
                {
                    if (text.Length == 0) return null;
                    throw new IOException("游戏插件返回了不完整的数据。");
                }
            }
            int newline = Array.IndexOf(_buffer, '\n', _position, _count - _position);
            int length = (newline >= 0 ? newline : _count) - _position;
            if (text.Length + length > maximumCharacters) throw new IOException("游戏插件返回了过长的数据。");
            text.Append(_buffer, _position, length);
            _position += length;
            if (newline >= 0)
            {
                _position++;
                if (text.Length > 0 && text[^1] == '\r') text.Length--;
                return text.ToString();
            }
        }
    }
}
