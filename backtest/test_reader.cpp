#include <iostream>
#include <fstream>
#include <string>

int main(int argc,char** argv)
{
    if(argc<2)
    {
        std::cout<<"usage: reader file\n";
        return 0;
    }

    std::ifstream f(argv[1]);

    if(!f)
    {
        std::cout<<"FAILED TO OPEN FILE\n";
        return 0;
    }

    std::string line;
    long long count=0;

    while(std::getline(f,line))
        count++;

    std::cout<<"LINES READ: "<<count<<"\n";
}
