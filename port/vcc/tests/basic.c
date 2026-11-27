int add(int left, int right)
{
    return left + right;
}

int complement(int value)
{
    return ~value;
}

int main(void)
{
    int value = add(20, 22);
    int count = 0;
    while (count < 3)
    {
        value = value + 1;
        count = count + 1;
    }

    int bits = (value & 15) | 16;
    bits = bits ^ 29;
    if (value < 40 || value > 50)
        return 1;
    if (!(value >= 40 && value <= 50 && value % 8 == 5))
        return 2;
    if (bits != 0 || complement(0) != -1)
        return 3;
    return 0;
}
